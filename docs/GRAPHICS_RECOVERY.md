# OpenGL context-loss recovery

Audit date: 2026-10-30. Applies to CR1-U, CRO-U, CR2-U and CR3-U.

## Confirmed gap

All four Android ports use OpenGL ES 3, with Vulkan disabled in their Android
builds. They previously ignored `SDL_EVENT_RENDER_DEVICE_RESET`. A replacement
context consequently left their texture, mesh, shader, uniform and vertex-array
names referring to the lost context. Continuing to draw could produce missing
art or a blank screen even though the gameplay state was still present.

The pinned SDL release itself establishes the recovery contract:
[`SDL_androidevents.c`, release-3.2.0](https://github.com/libsdl-org/SDL/blob/release-3.2.0/src/video/android/SDL_androidevents.c).
`android_egl_context_restore` tries to make the old EGL context current. When that
fails, SDL creates and makes a replacement current, then pushes
`SDL_EVENT_RENDER_DEVICE_RESET` with the window ID. The port must adopt that
context and rebuild its resources; creating another context or deleting old
names inside the new namespace would be incorrect. This was checked against the
local source of SDL's pinned release, rather than inferred from a newer API.

This confirms an engine defect and a plausible explanation for missing graphics
after Android resumes. It does **not** reproduce the user's particular device,
the reported early crystal-room crash, or establish that every white/black-screen
report has this cause. A normal pause that retains the context needs no rebuild.

## Implementation

- `RenderDevice::restoreAfterContextLoss` adopts SDL's current replacement
  context. The GL device reloads entry points, shader programs and uniform
  locations; recreates quad buffers/arrays, mesh buffers/arrays, live textures
  and generated mipmaps; and recreates CR2/CR3's material uniform buffer.
- Textures and meshes now have stable opaque client handles. Actual GL names
  remain private to the device. Existing references in the scene, menus, text
  caches and materials therefore remain valid after restoration, including
  secondary/normal/extra texture units and the untextured white fallback.
- Only live resources have CPU backups. Larger texture bases use lossless zlib
  compression at upload; small buffers use raw storage. Meshes retain their
  indices and most recently updated vertices so an animated pose survives.
  Destroying a resource releases its backup. Mip levels are regenerated rather
  than retained. This adds memory and some upload CPU cost; it avoids rereading
  archives or rebuilding gameplay during resume. All GL builds use this path;
  Vulkan does not acquire these backups.
- Filtering and vsync choices survive recovery. Anisotropy is clamped to the
  current device limit, which is queried again in the replacement context.
- Window-specific reset events are handled before further gameplay rendering or
  uploads. CR1/Omega defer an atomic request to their game thread; CR2/CR3 handle
  it in their main event loop. A failed restoration ends the app with an error
  instead of continuing to render with invalid resources.
- Lost-context object names are discarded without deletion. Shutdown deletes
  only resources belonging to the replacement context, including resources left
  live by a caller.

No save format, archive data, scene progression, language selection or Vulkan
renderer behavior was changed by this fix. No new whole-game decompilation was
needed to establish this source-level lifecycle defect.

## Verification

The new asset-free `gl-context-recovery` CTest creates an actual hidden GL
window, renders textured quads and textured/untextured meshes, destroys the
context, creates a replacement and restores the resources three times. It
requires identical pixels with no GL errors, tests a dynamic vertex update,
mipmapped/nonmipmapped textures, filtering above the hardware limit, released
resources and new uploads without handle collisions. Pixel backup round trips
are checked separately inside this test. The prior renderer fails the same
context-replacement experiment with GL errors and different pixels.

The test is built and run both as desktop GL and as GLES under ASan/UBSan in
all four repositories. All 24 CTests pass in each configuration, including the
previous importer, interrupted-input, scene-reset and CR3 feature tests. The GL
test returns skip code 77 when no suitable context exists; the local Mesa
runs actually execute and pass it, rather than skipping.

Real game scenes were also tested under GLES and ASan/UBSan with three actual
context replacements per data set, requiring identical rendered frames:

| Port | Data tested | Scene |
| --- | --- | --- |
| CR1 | Android 1.1.1, iOS HD 1.0.1, iOS 2.2.0, Vita | d0101 field, character and dialogue panel |
| Omega | Android 1.1.6, both available iOS releases, Vita | d0101 field, character and dialogue panel |
| CR2 | Android 2.1.2, iOS, Vita | d0101 field with c001 |
| CR3 | Android 1.1.3, iOS, Vita | t0108 entry cutscene, advanced three seconds past its initial fade |

That is 14 asset variants and 42 real-scene context replacements,
separate from the synthetic GL tests. Real-scene checks also require visible
nonblack pixels, so an initially faded-out scene cannot pass solely because its
blank frames match. CR1/Omega's fixture suppresses the wall-clock blinking
next-page arrow so pixel comparisons measure graphics restoration. The initial
fixture's blinking-arrow differences were not rendering failures. CR1/Omega explicitly enable the field background in their test fixture. CR3
uses its own c101/t0108 assets, not the CR2-only c001 model name; its initial
fade was identified by visual inspection and the corrected capture advances
through it before testing. CR2 uses its populated field rather than the small
standalone model, which falls below the field-oriented visibility threshold. Captures were inspected for visible scene content.

Reproduce the asset-free check on a headless Linux machine:

```sh
cmake --build engine/build --target gl-context-tests
SDL_VIDEODRIVER=offscreen ctest --test-dir engine/build -R gl-context-recovery --output-on-failure
```

Use `CR_GLES=ON` for CR1/Omega, `CR2_GLES=ON` for CR2, or `CR3_GLES=ON`
for CR3 when configuring a desktop GLES build. The real-scene checks use
`CR_SELFTEST_GRAPHICS=1` with CR1/Omega's `--selftest`, or with CR2/CR3's
`--capture` and a selected model/area. They require user-supplied game data.

Remaining device validation: background/resume with the OS actually discarding
the EGL context, orientation changes on physical Android hardware, and the
original report's exact save/route. No physical-device resume or EGL-loss trace
is claimed by these desktop Mesa context-replacement tests.
