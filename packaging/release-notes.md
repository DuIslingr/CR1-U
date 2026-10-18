# CR1-U v0.1.0

A from-scratch reimplementation of *Chaos Rings* (iOS / Vita / Android), first release: builds for
Windows, macOS, Linux and Android.

| File | Platform |
|---|---|
| `CR1-U-windows-x64.zip` | Windows 10+ x64 (Vulkan or OpenGL; FSR 3.1) |
| `CR1-U-macos-universal.zip` | macOS 11+, Apple silicon and Intel (Vulkan through MoltenVK, OpenGL; FSR 3.1, MetalFX on macOS 13+) |
| `CR1-U-x86_64.AppImage`, `CR1-U-linux-x86_64.tar.gz` | Linux x86-64 (Vulkan or OpenGL; FSR 3.1) |
| `CR1-U-android.apk` | Android 7+, arm64 / x86-64 (OpenGL ES 3.0) |

**The game's data** - any one of: the iPad *Chaos Rings HD* 1.0.1's `dataHD` folder (from
`ChaosRingsHD.app`), the iPhone 2.2.0's `data`, the Vita's `PCSG00497` folder, or the Android 1.1.1
release's `main.100.com.square_enix.chaosrings.googleplay.obb` with its APK. Put `dataHD` beside the
program (beside the `.app` on macOS, beside the AppImage on Linux), in
`Android/data/local.preservation.chaosrings/files/dataHD/` on Android, or pass `--data <folder>`.
The Options' Game page adds Vita or Android assets to an iOS copy.

**In this release:** the game playable with its own menus, Options pages for display, graphics
(render scale, MSAA, FSR 3.1 / DLSS / MetalFX), and the new Detail page (HD textures, smoothed
models, anisotropic filtering, texture filtering); HD textures you make yourself with
`tools/hd_textures.py` (waifu2x / Real-ESRGAN).

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
