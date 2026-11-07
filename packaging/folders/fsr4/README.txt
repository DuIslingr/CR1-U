Chaos Rings (CR1:U): AMD FSR 4 goes here (optional; Linux and Windows, an AMD GPU of the
RX 6000 series or later). It isn't included: AMD ships FSR 4 for Vulkan only as its own signed
Windows DLLs, whose licence lets them be passed on in that form under AMD's terms alone.

Put in this folder:
  amd_fidelityfx_loader_vk.dll     } AMD's FSR SDK 2.4 (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK,
  amd_fidelityfx_upscaler_vk.dll   } release v2.4.0: Kits/FidelityFX/signedbin/)
  amdvkffx64.dll                   AMD Software: Adrenalin Edition 26.10.1 or later (the Windows driver
                                   package; inside the installer: Packages/Drivers/Display/WT6A_INF/B*/) -
                                   FSR 4 itself; without it AMD's SDK offers only FSR 3.1
  libffx_vk_linux.so               Linux only: ffx-vk-linux (github.com/DuIslingr/ffx-vk-linux, MIT), which
                                   maps AMD's DLLs natively - no Wine

Then Options > Graphics > Upscaler: FSR 4 (or --upscaler fsr4). The game's log says which FSR
AMD's SDK picked ("FSR 4.1.2: AMD's upscaler from ..."). Without these files the game's own
FSR 3.1 (built from AMD's source) is what it has; nothing else changes.
