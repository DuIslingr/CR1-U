# Chaos Rings HD — binary and data formats

## The binary

- `ChaosRingsHD`: Mach-O ARMv7, **ARM mode** (not Thumb), `cryptid 0` (unencrypted),
  **not stripped** — 2,711 defined symbols, 2,134 functions.
- Built with llvm-gcc 4.2, iPhoneOS 3.2 SDK, by Media.Vision.
- Imports: UIKit/Foundation (thin), OpenGL ES **1.1** only (incl. `OES_matrix_palette`
  for skinning, `OES_framebuffer_object`, PVRTC), OpenAL (SFX), AudioQueue +
  AudioFile (streamed music), CoreGraphics (text rendering into bitmaps).

### Code map (by symbol prefix)

| Prefix | What it is |
|---|---|
| `mce*` (~600) | **Mascot Capsule** 3D engine by HI Corporation: figures, actions (animation), bone/morph/node deformers, appearance (materials), camera, lights, particles, draw queue |
| `hwal*` | Engine's GL ES 1.1 hardware abstraction layer (`hwalCreateTexture_ES1`, …) |
| `HI_LZW_*`, `LZWIstream_*`, `LMemIstream_*`, `hiBmpDecoder_*`, `hiMalloc` | HI Corp. runtime: compression, streams, memory |
| `read_*`, `file_load_*` | Game-side asset loading (`read_mdl_mem_mct`, `read_mdl_mem_mca`, `read_mdl_mem_mcm`) |
| `mdl_*` | Game-side model/animation wrapper |
| `btl*` | Battle system |
| `_Evt*`, `fld*`, `npc_*` | Field events, field, NPCs |
| `Mu*`, `_men*`, `ComUty_*`, `_Common_*` | Menus / UI |
| `pad_*` | Touch → virtual pad input |
| `CRView`, `EAGLView`, `AppController`, `AppDelegate`, `GBMusicTrack` | Obj-C shell: GL view, touch, lifecycle, music streaming |

## Compression: HI-LZW

`HI_LZW_InitializeDecode` / `HI_LZW_DecodeByte` (0x8abf4 / 0x8a9c0) — identical to
GIF image-data LZW:

- byte 0 = minimum code size N (2..11, always 8 in practice)
- data is in sub-blocks `[len u8][len bytes]…`, a 0-length block terminates
- codes read **LSB-first**, start at N+1 bits, grow to 12 bits max (4096 entries)
- clear = `1<<N`, end-of-information = clear+1

Implementation: `tools/hilzw.py`.

## `.mct` — texture

Loaded by `read_mdl_mem_mct` → `mceLoader_load_p` → `hwalCreateTexture_ES1`.

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | char[4] | `MCT_` |
| 0x0a | u32 | file size − 10 |
| 0x1a | u16 | texture id (e.g. 100 for `100.mct`, 0 for most) |
| 0x1e | u32 | decoded payload size (0 if raw) |
| 0x22 | u16 | width |
| 0x24 | u16 | height |
| 0x26 | u16 | 1 = HI-LZW compressed, 0 = raw |
| 0x28 | u16 | pixel format (below) |
| 0x2a | … | payload (palette first for paletted formats) |

Pixel format = index into the GL tables at 0xd4e20 / 0xd4dac:

| fmt | GL ES format | Used by |
|---|---|---|
| 1, 21 | RGB / UNSIGNED_BYTE | 36 files (portraits, item art) |
| 2, 22 | RGBA / UNSIGNED_BYTE | 10 |
| 3 / 4 / 5 | RGB565 / RGBA5551 / RGBA4444 | `window_hd.mct` (5) |
| 6–10 | `GL_PALETTE8_{RGB8,RGBA8,R5_G6_B5,RGB5_A1,RGBA4}_OES` | 6: 111, 7: 147 |
| 11–15 | `GL_PALETTE4_{RGB8,RGBA8,R5_G6_B5,RGB5_A1,RGBA4}_OES` (high nibble = first texel) | 11: 1, 12: 6 |
| 16 / 17 / 18 | LUMINANCE / ALPHA / LUMINANCE_ALPHA | 17: 1 |
| 25 / 26 | PVRTC RGBA 4bpp / 2bpp | 25: 114 (character textures) |
| 27 / 28 | PVRTC RGB 4bpp / 2bpp | unused |

Rows are stored top-to-bottom. PVRTC blocks are in Morton (twiddled) order.
Implementation: `tools/mct.py` — all 441 files decode.

### Appearance body (material) — reader 0xa69a0

Layout verified on all 1,503 records (every byte consumed).

```
[class == 0: u32]
u32  colour[4]         bytes R,G,B,A: 0 diffuse (also the unlit glColor), 1 ambient,
                       2 emission, 3 specular (hwalSetAppearanceColor_ES1)
u32  silhouette colour
u8   shininess, alphaThreshold, blendSrc, blendDst, shading, culling, ?
u16  drawOrder (signed)
u32  properties        bit3 depth test, bit4 depth write, bit1/2 colour mask,
                       bit5 perspective hint, bit7 polygon offset, bit8 two-sided lighting
f32  polygonOffset factor, units, silhouette width
2 × texture unit {
   i32  textureRef      = MCT header id (0x1a) of a package texture; -1 none
                          (4 materials use a 1-based package index instead)
   [class > 0: u32]  [class > 1: u8 uvStream]
   u8   texBlend, wrapS, wrapT, magFilter, minFilter, textureType
   u32  texture blend colour
   f32[3] × 4  UV transform,  f32[3] UV scale
}
[class > 1: u32]   u32 refGid
```

| field | codes |
|---|---|
| blendSrc | GL ES 1.1 source list: 0 ZERO, 1 ONE, 2 DST_COLOR, 3 1-DST_COLOR, 4 SRC_ALPHA, 5 1-SRC_ALPHA, 6 DST_ALPHA, 7 1-DST_ALPHA, 8 SATURATE |
| blendDst | 0 ZERO, 1 ONE, 2 SRC_COLOR, 3 1-SRC_COLOR, 4 SRC_ALPHA, 5 1-SRC_ALPHA, 6 DST_ALPHA, 7 1-DST_ALPHA |
| | blending disabled for (1,0). Data: (4,1) additive 872, (4,5) alpha 495, (1,0) opaque 134 |
| alphaThreshold | alpha test `GEQUAL threshold/255`, off when 0 |
| culling | 0 none, 1 front, 2 back; front face CCW (hwalReset_ES1). Meshes are CCW-outward |
| texBlend | 0 REPLACE, 1 ADD, 2 BLEND, 3 DECAL, 4 MODULATE (table 0xcb668) |
| wrap | 0 REPEAT, 1 CLAMP_TO_EDGE |
| filter | 0 NEAREST, 1 LINEAR, 2–5 mipmap variants (1,501 of 1,503 materials use 0) |

Effect materials often have diffuse alpha 0 at rest; their colour is animated by the
non-skeleton (9-segment) actions, whose keyType 12 kind-4 tracks look like RGBA.

### TransformTree body (skeleton) — reader 0xaa820

```
u32  refGid
u16  nNodes
nNodes × {                                   (91 bytes each)
   u16    parent        0xFFFF = root (a tree may have several roots)
   f32[3] axisX, axisY, axisZ, translation, scale
   u8     flags
   f32[3] minAngle, maxAngle    f32 damping   (IK limits; ±1 / 0 defaults in shipped data)
}
```

Local matrix (column-major, column-vector convention): `[X·sx | Y·sy | Z·sz | T]`;
`world[i] = world[parent] · local[i]`. Verified: joints land within ~2% (of model
size) of the vertices they drive; transposed axes or no hierarchy give 15–20%.

### NodeDeformer body — reader 0xa9eac

```
u16  nNodes
u32  ?
nNodes × { i32 objectId, u16 node, u8 visible }     (exact for all 335 models)
```

Attaches objects (Bone records, mesh Figures) to TransformTree nodes. `visible` is the
object's initial visibility: 21 mesh figures start hidden (editor helpers such as the
untextured magenta mesh in d0301/d0302, and effect parts). The actions whose segment
count equals the NodeDeformer entry count (e.g. m201's 75-segment, keyType-12-only
actions) are probably per-entry visibility animation. **A mesh's
vertex positions are in the local space of the node its Figure is attached to**
(e.g. c102's mesh is attached to node 66, offset −26.3 on Z).

### Figure body — reader 0xa977c

```
u16  nIndexBuffers
i32  indexBufferId[nIndexBuffers]
i32  vertexBufferId, i32 ?, i32 ?, i32 boneDeformerId, i32 ?, i32 ?, i32 ?
```

The skeleton figure has 0 index buffers and references the TransformTree id,
NodeDeformer id and a Camera id instead.

### Skinning

`skin(bone) = W_anim[bone] · W_rest[bone]⁻¹ · W_rest[attachNode]`, linear blend with
the BoneDeformer weights. Checked: c102 p95 edge stretch at mid-animation 0.75%
with the attach node vs 27% without.

## `MCA_` — animations (actions)

Reader 0xa7ec4 (container) / 0xa8094 (action); selected by the `MCA_` entry of the
signature table at 0xcf19c. Found standalone (`*.mca`) and inside `.bin` packages.

```
'MCA_'  u32 version, u8 option (value type, 0 = f32), u8[3], u16 nActions
action record: 16-byte header, then
   u32 ?, u8 compressed, u32 decodedSize, f32 duration (frames), u8, u8, u16 nSegments
   (HI-LZW) nSegments × { u16 nTracks; nTracks × track }
track:  u8 keyType, u8 kind, u16 nKeys, u16, u16
   kind <= 9 : nKeys × { u8 mode, u8 n, f32 time, f32[kind × n] }
               n = 1 value only, 3 = value, in-tangent, out-tangent
   kind >= 10: nKeys × { u8 mode, u8 fmt, f32 time, packed values }  (unused in shipped data)
```

All 205 `.mca` files / 410 actions parse with every byte consumed. Segment *i*
animates TransformTree node *i* (frame-0 translations equal rest translations
exactly); actions whose segment count differs from the node count animate other
objects (9-segment actions: mostly keyType 12 scalars). Exception: a list's first action
("N.1", the motion) can be a segment or a few short of the node count — Escher's gene casts
(lists 10, 11: 56 of 58), m103's staged attack (list 14: 148 of 149): they drive the first
nodes in order and the rest stay at rest. Some have more segments than nodes (m104's lists
2, 4, 5, 8, 9, 11, 13: 150 of 148; c109, c111: one more); neither head- nor tail-aligned
mapping poses those convincingly, so they're left out (the slot stands).

| keyType | used kind | applied by | meaning |
|---|---|---|---|
| 1 | 3 | `setTranslate` | translation (absolute) |
| 5 | 3 | `setRotateEulerXYZ` | rotation, Euler **turns** (1.0 = 360°), R = Rz·Ry·Rx |
| 11 | 3 | `mulScale` | scale (0.001 is used to hide parts) |
| 12 | 1 | — | visibility/alpha (1.0) |

(keyTypes 2–4 orient/roll and 6–10 other Euler orders exist in the engine but are
not used.) Key `mode`: 1 linear, 2 Hermite, 3 step. Hermite between keys k0,k1 with
`s = (t − t0)/(t1 − t0)`: `p = h00·v0 + h10·out0 + h01·v1 + h11·in1` — tangents are
**not** scaled by the interval. Times are in frames within `[0, duration]`, a frame a game frame
(`mdl_actctl_prc`: + 1) - 20 a second: the game runs everything at 20 frames a second (1.0.1 and
2.2.0 schedule EAGLView's `drawView` -> `CRView prc` -> `gameapp_prc` on an NSTimer of 0.05 s;
Android paces `onDrawFrame` to 50 ms and adds 50 ms of play time a frame).

Node pose: start from the rest axes/translation, a rotation track replaces the
3×3, a translation track replaces T, then scale (track value, or the rest scale if
the node has no scale track) multiplies the axes.

## `.bin` (character/effect) — model package

Parsed by `read_mdl_mem` (0x6d9c8). Starts with a table of `(u32 offset, u32 size)`
pairs, offsets relative to the file start:

```
entry 0                 MCM_ model
entry 1..k              MCT_ textures, until an entry with offset 0
(zero entry)
entry k+2..             MCA_ animations, until an entry with offset 0
```

## `MCM_` — model block

Loaded by the HI "MCx" loader (`__readModelData`, 0xa8a8c — selected by the
4-byte signature table at 0xd0174; strings at 0xc7874). Not the J2ME-era MBAC
format, despite sharing an engine.

All values little endian and **unaligned**. Header: `MCM_`, u32 version, **u8 option**,
u8[3]. The option byte is the file's real-number format for every "dynamic"-typed field
(skeleton, skin weights, UV scale, material floats): **0 = f32, 1 = s32 16.16 fixed point**
(36 models use 1: c3xx, c41x, several d0xxx, f0000blk04, m203*). `MCA_` blocks carry their
own option byte with the same meaning (all shipped ones use 0). After the 12-byte header the block is
exactly **12 sections**, each `u16 count` followed by `count` records. Every record
begins with a 16-byte header `u32 size (incl. header), u32 class, u32 id, i32 user`
so the whole block can be walked without understanding records
(`tools/mcm_walk.py`). Objects reference each other by `id`.

| # | Section | Reader | Notes |
|---|---|---|---|
| 0 | Appearance (material) | 0xa69a0 | see below |
| 1 | VertexBuffer | 0xa6008 | see below |
| 2 | IndexBuffer | 0xa5954 | see below |
| 3 | Light | 0xab378 | |
| 4 | Camera | 0xaafb0 | perspective/parallel, fog |
| 5 | GeometricShape | 0xaadc0 | |
| 6 | Bone | 0xaad00 | header-only records (ids) |
| 7 | TransformTree | 0xaa820 | node hierarchy, rest transforms, angle limits |
| 8 | BoneDeformer | 0xaa3d8 | skin weights, HI-LZW compressed |
| 9 | MorphDeformer | 0xaa10c | |
| 10 | NodeDeformer | 0xa9eac | rigid attachment of meshes to nodes |
| 11 | Figure | 0xa977c | binds shape + tree + deformers |

Record bodies are read by a generic table-driven field reader (0xa3750): each
reader builds 20-byte descriptors `{dest, type*, count*, alloc, subreader}`; the
type id indexes 15 primitive handlers (jump table 0xd01d4):

| id | reads | id | reads |
|---|---|---|---|
| 0 | f32 | 8 | RGB as u32 bytes /255 |
| 1 | s32 16.16 fixed → f32 | 9 | u8 → int |
| 2, 6 | u32 | 10 | u8 → f32 |
| 3, 5 | u16 | 11, 12 | u16 → u32 |
| 4 | u8 | 13 | 16-byte element |
| 7 | RGBA as u32 bytes /255 | 14 | list of nested records |

`tools/descx.py` reconstructs these tables from a reader's code.

### VertexBuffer body

```
u8   compressed        1 = rest of record is HI-LZW
u32  decoded size
8 × { u16 count, u8 dataType, u8 components }   attribute slots
     0 position  1 normal  2 colour?  3 ?  4 ?  5 texcoord0  6 texcoord1  7 ?
     (if header.class > 0, an extra u8 follows slot 4)
2 × real[4]            texcoord scale/offset per UV set (e.g. 2^-14, bias u, bias v, 0)
data                   each non-empty slot: count × components × sizeof(dataType)
```

dataType: **0 = f32, 1 = s32 16.16 fixed, 2 = packed RGBA8 (4 bytes), 3 = s16**. Slot 4 is
the per-render-vertex **colour** (type 2, 1 component; 40 models have non-white colours).
The attribute arrays must sum exactly to the decoded size — verified for every vertex
buffer in the game (an earlier 1-byte guess for types 1/2 shifted UVs and positions).

### IndexBuffer body

```
u8   compressed        1 = rest is HI-LZW
u32  decoded size
u8   primitive type    GL enum values: 4 = triangle list (m201*), 5 = triangle strips
u8   has user data
u16  (must be 0 for indexed buffers)
u16  nPrims
u16  nIndices
u32  appearance id
i32  ?
data s16[nPrims] strip lengths, u16[nIndices] indices
```

Indices address the **render-vertex space** (one slot per UV; e.g. c101: 1596
positions, 2194 UVs, indices 0..2193). Positions are shared between render
vertices; the mapping lives in the BoneDeformer (below).

### BoneDeformer body

```
u8   compressed        1 = rest is HI-LZW
u32  decoded size
u16  nPositions        == VertexBuffer position count
u16  nBones
data u16[nBones]       TransformTree node index of each bone
     nPositions × {
        u8   k             influences (<= 4)
        u8   dup           number of consecutive render vertices using this position
        f32  weight[k]     (type comes from a loader option; f32 in all shipped files)
        u8   bone[k]       index into the bone list above
     }
```

**Render vertex → position mapping**: render vertices are grouped by position, so
`position_of = repeat(arange(nPositions), dup)` (`__mceIndexBuffer_makeThinSkinArray`
builds the same table by prefix-summing `dup`). Verified: `sum(dup)` equals the UV
count for every skinned mesh, and strip edges become short (c101 median 1.4 vs.
15.4 for random pairs). Meshes without a BoneDeformer have equal counts and an
identity mapping.

## Field maps

A field `dXXXX` (74 of them) is a **pre-rendered 2D background** with 3D characters
walking on an invisible collision mesh:

| file | role |
|---|---|
| `dXXXX-b00a00.mct` | 1024×1024 background (PAL8_RGB8) — `FieldScene::drawBack` |
| `dXXXX-f00a00.mct` | optional foreground overlay drawn over characters — `drawFront` (9 fields) |
| `dXXXX_10.cmp` | collision mesh (below) |
| `dXXXX.bin` | optional 3D props, already in world coordinates |
| `dXXXX_fog/_snow/_sun.mct` | weather/light layers |

`fldread_prc` (0x42ce4) loads the map's event file and collision file through name
tables indexed by the area (`+0x35526`) and map (`+0x35528`) numbers. The per-stage
settings record (`npc_get_stgmapctl`, 0x44888) lives **in the event (`.evt`) data**:
a list of 52-byte records terminated by `0xFFFF`, followed by the stage record whose
`+24` f32 is the camera FOV (degrees; `/360` → turns) and `+40/+42` s16 the view size.
`FieldCamera::setCamera3D` builds `gluLookAt(eye, target, up=(0,1,0))` from live
camera state, far plane 2000 (per-map overrides), near = distance/10 (min 10), and
`mceCamera_setPerspectiveFov(aspect = viewW / viewH in whole numbers (`__divsi3`), FOV = +24 / 360)`
into the viewport below - also for a script's camera in a room without a painting (the opening's
map 113: a 1024 view, doubled to 2048 × 2048, of which the 768-row screen shows the middle - about
22° of its 54°). `FrmSetCam` / `FrmAdjCam` read only the camera actors' first three words; their
fourth (e.g. 37.91 → 32.75 in the opening's first shot) is unused. 2.2.0 centres the same viewport
on its own screen size (G+0x3c9c8), so the iPhone's shorter screen shows less of it.

## `.cmp` — field collision

Reversed from `Cmp::CmpBase::init` (0x36c8), `Cmp::TreeKD::init` (0x5fac),
`Cmp::TreeQUAD::init` (0x616c). All 114 files parse exactly (files are padded to a
multiple of 16 bytes); every vertex index is in range. `tools/cmp.py`.

```
0x00 u32  tree type          0 = KD tree (all shipped files), 1 = quad tree
0x04 s16  nVerts   0x06 s16 nPolys   0x08 s16 nNodes
0x0c f32  3.0 in every file (probably the collision sphere radius)
0x20 nVerts × vec3 f32        world coordinates, Y up
     nPolys × 32 bytes:
        f32[3] normal
        s16[4] vertex indices (4th unused for triangles)
        s16    vertex count (3 or 4)
        s16    attr0  ~300 distinct values — probably area / event-zone id
        s16    attr1  1 = wall (normal not up); floors 0/256/512/768/1024 (surface type?)
        s16    attr2  always 7
        4 bytes padding (0xAFAFAFFF)
     nNodes × node (KD 32 bytes, QUAD 40): +0 index-list pointer slot,
        +4..+10 four s16 counts, +24 s16 (child/leaf marker)
     per node: sum(counts) × s16 polygon index
```

Data: 2,716 floor polygons (normal.y > 0.7), 3,860 walls, 1 ceiling.

## Event files (`.evt`)

29 files, one per **area**: the four story routes `j001/j003/j005/j007` each have
dungeons `_d01`–`_d05` and a town `_t01`, plus `j001_t99`, `j001_p01` (puzzle rooms),
`j001_t91`, `j001_d91`. Tables in the binary (extracted by `tools/gen_tables.py` into
`engine/src/game/stage_tables.cpp`): area → event file, area → first map number,
map number (0..129) → collision file. A map's index inside its area's event file is
`map − firstMap(area)`.

Header (`init_evnt`, 0x80254): u32 section offsets (from file start) at 0x00–0x18 and
0x28/0x2c. Section 2 (`+0x08`) is a u32 offset per map of the area; at that offset is a
list of 52-byte records ending with one whose first u16 is `0xFFFF`, followed by the
**stage record**:

```
+0  f32[3] camera eye      +12 f32[3] camera target   +24 f32 FOV (degrees)
+28 f32[3] usually 0
+40 s16 viewW, s16 viewH   (1024×1024 = background size; 512×512 for town maps; −1 special)
+44 s16 x, y, w, h         placement rect of the background inside the view
                           (usually 0,0,1024,1024; d05xx tower fields 256,0,512,1024)
+52 s16 x0, y0, x1, y1     0 in the files: fldread_prc sets them to the rect's corners (the scroll limits)
+60 s16 background image index   +64 s16 prop index   +68 s16 player radius   +70 s16 look height
```

On the iPad `fldread_prc` doubles +40..+50 once per map (painted or not), so a 1024 view is 2048 screen pixels.
`FieldCamera::setCamera3D` puts the 3D viewport (and the painting) at ((1024 − viewW) / 2 + offX,
(768 − viewH) / 2 + offY) (GL, y up), viewW × viewH. `setScrollPlayer` (every frame while the
camera follows the player) projects the player's position + (0, 7 + look height, 0), clamps it to
[x0 + 512, x1 − 512] × [viewH + 384 − y1, viewH − 384 − y0] and sets offX = viewW / 2 − x,
offY = viewH / 2 − y. `setScroll2D` (view pixel, y down) / `setScroll3D` (a world point) do the same
with a rate, and only with the camera free. Painted fields' script cameras: `FrmAdjCam` slides the
offset to a shot's (x, y) / 100 over its frames (`ScrollAutoCtl`), `FrmPanCam` by twice its (dx, dy),
`FrmSetCam` on the field (flags word bit 64) takes the 3D path (SetScroll(0, 0), eye moved);
the 3D rooms (maps 13, 29, 46, 63, 88, 113) always use the 3D path. `Mapscroll` (op 31):
u16 who (0xFFFF the running NPC, 0 the player), s16 rate% → setScroll3D; who < 0: s16 x, y, rate%
→ setScroll2D (8 bytes in all).

Scene effects: call 2020 (`Uty_ScnEffectRead`) loads set var 0 into Tex2d slots 49..72 (0 d01: snow
61, fog 64, d0117 fog 51; 1 d02: sun 69 (512²), fog 53; 2 d03: fog 49; 3 d04: rain01 55, rain02 57,
fog 59; 4 d05: tower backgrounds 66..68, fogs 71, 72; 5 p01: fog 70 (512×256); each + 1 is its
"(128)" copy, used in battles), call 2009 (`Uty_ScnEffect_Init`, stack 0 on / off, 1 the effect)
starts or stops effect 1..20. `ScnEffectProc` draws each effect on (`tex2d_add` z −50, at (−16, −32)
512 × 384 in the 480 × 320 space, UV = a 512 × 384 window of the wrapping texture, sx / sy = half
the scroll offset): 2: d0307 fog (t − sx, t/2 − sy) and (t + 128 − sx, 384 − t/2 − sy), t ≤ 512;
3: d0117 fog (−0.64a − sx, 0.64a − sy), (0.5818b − sx, 0.5818b − sy), a ≤ 400, b ≤ 440; 4: rain01
(0, −51.2t − sy), rain02 (−13.251t − sx, −49.455t − sy), fog (−1.28t − sx, 1.28t − sy), t ≤ 200;
5: fog (−6.4a − sx, 3.2a − sy), snow (−12.8b − sx, −12.8b − sy) and (128 − 6.4b − sx, −6.4b − sy),
a, b < 80; 9: d0209 fog (−2.133a − sx, −2.133a − sy), (128 − 1.422b − sx, −1.422b − sy), a ≤ 120,
b < 181; 10: the sun, 512 × 320 at UV 0; 11: p01 fog 512 × 256 at y 128, (2.133t − sx, 0), t < 241;
1: slot 47 every other frame in four mirrored 256 × 192 quarters; 6..8 / 12..20: the tower
(`Space_Stop` / `Up` / `Down`, variant 0 for 6..8, 1 for 12..14, 2 for 15..20; 6 / 12 / 15 / 18
stop, the next up, the next down): `tex2d_add` z −1998 (behind the 3D, depth tested), the
variant's 512 × 1024 painting (slot 66 + v) as a 512 × 384 window at UV (0, 1.024 r), r the rise
(G 0xddfb4: +1 a frame going up, wrapped past 2000; −1 going down); variants 1 / 2 add their fog
(slot 70 + v, 512 × 256 at the top) twice, at UV (±1.066 d − sx, 0), d the drift (0xddfb8, +1 a
frame, 0 past 480). `ScnEffectProc` runs every frame wherever the game is (`gameapp_prc`): effects
2..5 and 9 draw only while G+0x2c0 bit 6 (the field: their own slot) or bit 5 (a battle, set by
`btl_main`: the "(128)" copy in the next slot); 10, 11 and the Space ones draw in both; the fogs
over the tower (variants 1 / 2) and effects 2..5, 9, 11 check the Options' screen-effect state
(G+0x31dd4, `__GetScreenEffectState`; nonzero skips them, their counters standing still). It is
Options' Environment Effects (`__menuu_OptionSet` +0x37 On / +0x38 Off; `__MuOptionSetInfo`
stores +0x37 != 1); +0x39 / +0x3a, G+0x31dd3 bit 0, is Lock Screen Orientation. Layer alphas come from `UpdateRandom`: per channel fade in by `inc`
for `fadeIn` frames, hold random(holdMin..holdMax), fade out, hold `hold2` - effects 2 / 9: 135 ↔
255 (3 × 40), 4: 55 ↔ 255 and 135 ↔ 255, 5: 55 ↔ 255, 135 ↔ 255 and a third channel fixed at 135.

The background image is the scene rendered from this camera at aspect `viewW/viewH`:
projecting each field's collision through it puts a median 96% of vertices inside the
frame (49 of 80 fields ≥ 90%).

Layer file names vary (the game keeps a flat list at 0xd3fb8):
- `dXXXX-b00a00.mct` for most fields;
- `tXXXX_b00a00.mct` (underscore) for the towns and for d0512/d0524/d0525;
- a plain `dXXXX.mct` for d0108, d0117, d0208, d0219, d0307 and d0320.

Foregrounds are `-f00a00` or `_f00a00`. d0101/d0117/d0209/d0307/d0401/d0507/d0511/p0001 add
`_fog` layers (not drawn yet). Stage record `+62` indexes the image list and `+64` the
field-prop list (`_stgalphaname`, the 27 `dXXXX.bin`/`t0101.bin` props). The 512×512 fields
(d0108, d0117, d0208, d0219) have a 512×512 view.

Layers are drawn at native pixel size relative to the stage view (`side / viewW`), centred. The d05xx tower
fields (d0501–d0520) only use a 512-wide strip at x 256–768. It is stored as either a
512×1024 image (d0501/d0507/d0511) or a 1024×1024 image with black sides. Their
foregrounds are 1024×1024 with alpha only in that strip. Stretching the 512-wide images
to the full square caused the d0516–d0518 misalignment; drawn natively, they align.

### Sections

`init_evnt` turns each header offset into a pointer at a fixed global:

| Header | Global | Contents (users) |
|---|---|---|
| +0x00 | +0x31de4 | script entry table: u32 per script (1000 + a self-pointing sentinel); `EvtStart(n)` |
| +0x04 | +0x31de8 | 9 offsets of scripts the game starts (`EvrStart`: 2 the battles' tutorials (in battle, var 1 the key) / items, 3 camp, 4 shop, 5 room entry), then code |
| +0x08 | +0x31dec | u32 per map of the area → that map's NPC list + stage record; the lists follow the table, so indexes past the first list are junk |
| +0x0c | +0x31df0 | NPC table: 40-byte rows indexed by NPC id (`npc_setup`, treasure boxes) |
| +0x10 | +0x31df4 | warp table: 20-byte entries (`EvtWarp`) |
| +0x14 | +0x31df8 | encounter tables (`fldencount_*`) |
| +0x18 | +0x31dfc | cutscene frame data (`EvtFrm*`); indirect through its first u32 |
| +0x28 | +0x31e00 | message text (`EvtMsg`, font code) — not decoded here |

The file base itself is kept at +0x31de0. Every script address in the bytecode is relative to it.

### NPC lists and the NPC table

A map's list holds 52-byte records ending with a u16 `0xFFFF`:

```
+0  u16 id          the NPC table row, and the script it runs
+4  f32 x, y, z     +16 f32 ?     +20 f32 angle (radians; npc_setup divides by 2π)   +24 f32 ?
+28 f32 box w, h, d                +40 f32 offset x, y, z
```

NPC table row (40 bytes, s16 fields):

```
+0  show flag (-1 none)   +2 hide flag   +4 model (< 0: no model)   +10 = id   +14 kind
+38 scale % (-1 = 100)
```

`kind` bit 0 together with bit 15 (`0x8001`) marks a **touch trigger**. Model-less touch triggers
are the room exits.

`RectWrapper::checkEventKick` tests the player's x/z (y is ignored) against the box
`centre ± size/2`. The centre is the NPC position plus the offset, and model-less NPCs get the
offset twice: once in `npc_setup` and once in the check. On entering a box it calls
`EvtStart(id)` once; the last box is remembered at +0x35534 until the player leaves it.

### Warps

`Warp` (opcode 17, 12 bytes: six u16) writes the field start globals that `FieldPCCtrl::reset`
reads: position at +0x2f940.., angle in degrees at +0x2f94c.

- First u16 = 0xFFFD: look up the warp table by the running event's number (+0x322ca).
  A 20-byte entry is `f32 x, y, z, s16 key, s16 area (-1 = same), s16 map, s16 angle°`, and the
  table ends with key -1. Entry 0 is used if its key is -1 or matches; otherwise the first later
  entry whose key matches.
- First u16 = 0xFC19: return to the title screen.
- Otherwise the six u16 are area (0xFFFF = same), map, x, y, z and angle. x, y, z are read
  unsigned.

Map numbers are global (0..129). The four routes' dungeons share map numbers, so the area (event
file) is part of the destination. If the destination map equals the current one, only the player
moves; otherwise the field reloads.

In practice: 534 touch triggers, of which 460 warp on a new game (all to known maps). The
self-test fires all 459 active exits and every arrival lands on a floor. Five destinations sit
up to 6 units past a floor edge and are snapped to the nearest floor.

### NPC models and treasure boxes

NPC table `+4` indexes a 200-entry model-name table in the binary (0xd44e0 via 0xcf0ac,
extracted into `kNpcModelFile`):

| Range | Contents |
|---|---|
| 0–31 | field objects `f0000*`: barrels, blocks, boxes, chains, CPU, doors, jewels, rocks, switches, … |
| 32–98 | characters and NPCs (`c1xx`–`c4xx`, town folk `n001`–`n008`) |
| 99–196 | effects (`fe*`, `be*`, `ee*`) |

How `npc_setup` places a model:
- **Position:** the record position, plus the offset only for models 0/1.
- **Rotation:** about Y by record `+20` (radians).
- **Scale:** NPC table `+38` / 100.
- **Loaded only if** the show flag is −1 or set, and the hide flag is −1 or clear.
- **Parking:** cutscene actor slots wait at y 995–1000 or at the origin until `FrmSetc2` moves
  them (town folk 401–408 in most maps).

In the shipped data, 1,526 NPCs are drawn on 358 (area, map) pairs.

Treasure boxes (`TreasureBoxManager`):
- **When:** `__menuu_WldChoiceClr` stores the chosen band (+0xc10 + route) and sets G+0x2c0 bit
  0x40000000; the next event file loaded (`fldread_prc`) clears it and, for any area but 0, 1, the
  Arks (7, 13, 19, 25) and 26, calls `_InitDungeon` -> `randomSelect`. So a dungeon's boxes are
  drawn again each time the World Select leads into it; the draw (15 `{id, item}`) is in the save.
- **`randomSelect`** (seeded with `time()`) builds the candidates with `PickUpNpcTbl`: every
  second NPC id in [100, 140) and [140, 180) whose table `+18` bit 0 is set. It drops random ones
  until 7 and 8 remain, then gives them items. Box ids are even; id + 1 is the box's marker
  model (`c109`, sitting 8.5 above the box).
- **`setup`** (on each map) clears flags 3100–3259. For each box NPC on the map that is in the
  saved list `{id, item}`, it sets the box's show flag. If `item == 0xFFFF` (taken: `setOpened`),
  it also sets the marker's hide flag. Ids 180–799 are not boxes.
- **Gold boxes** (table +18 bit 1; `f0000gbx00`, ids 60–98 and 800 on): with the band at 2 or more
  (clamped 0–19), an item dealt from pool slot 9–14 goes into the box's gold counterpart - box
  100–139 + 700, 140–179 − 80 - when the area has one (id within the table, bit 1 set). Its lid
  script asks for a key (`Uty_ChkItem` of item 2; the shop sells them from level 3) and takes it.

All 310 boxes shown in the shipped data carry their marker on top.

### Solid NPCs and examining

The NPC table's `kind` (+14) bits:

| Bit | Meaning |
|---|---|
| 0 + 15 | touch trigger (`checkEventKick`) |
| 1 | solid (`checkHitWallToSphere`), unless bit 2 is also set |
| 3 | examinable: objects get the "!" badge (`checkFollowWindow`), people with bit 4 the talk badge (`checkFlagWindow` → `FlagWindow`); both start the NPC's script |

- **Solid box** (`RectWrapper::init`): the record's box, `offset ± size/2`, rotated by the NPC's
  angle (`RepeatPI`, `cosf`/`sinf`) and placed at the NPC.
- **Examine range:** the box centre (position + offset) ± (size/2 + player radius + 1.5).
- **`FollowWindow::TouchFollowWindowUtil(n)`:**
  - Treasure box (table +18 bits 0/1): var 11 = n, var 10 = item (table +12, or
    `TreasureBoxManager::getItemNo`), then `EvtStart(n + 1)`. The lid's script plays the lid's
    open animation, then calls `AddItem(var 9)` and `SetOpenedTresureBox(var 11)`, shows the item
    window, sets the lid's hide flag, fades the lid out and removes it.
  - Anything else: `EvtStart(n)`.
- **Crystals (`c111`)** run a warp script whose warp-table key is their own id (e.g. key 1 →
  the town hall, key 3 → the next room).

Item pools (`randomSelect`, second half):
- **Table:** one of `_TBOX_TABLE_A/B/C` (`rand() % 3`), each 10 levels × 13 items. The level is
  the route's World Select band (+0xc10 + route, clamped 0–9).
- **Specials:** two more from `_TBOX_PAIR_TABLE_A/B/C` [level][route].
- **Dealing:** the 15 are reordered - each in turn taken out and put back before a random one of
  the rest (`rand() % 14`) - and given to the selected boxes in order (the 7, then the 8);
  `getItemNo` returns 1 when a box isn't found or is taken.
- **Names:** item n's name is system message 51000 + n - 1 (`sys.evt` block 0; item 1 is
  message 51000), its help text 52000 + n - 1 (block 1). The shop's prices fix the offset: item
  201 costs 450 and the reference playthrough sells "Trusty Longsword" (51200) for that.

Self-test: 290 boxes give 53 different items, and none gives item 0.

Native calls (`Call id` with id > 2000) use the table at 0xd3b80 (12-byte entries: init / proc /
put), e.g. 2001 `touch_up_chk`, 2004/2010 `mdl_fadeout`/`mdl_fadein`, 2005 `GetItem` (window),
2009 `ScnEffect`, 2011–2013 `AddItem`/`DelItem`/`ChkItem`, 2019 `mdl_set_alpha`, 2032
`SetOpenedTresureBox`. They read their arguments from the current frame (`arg(0)`…).

Self-test results:
- Walking into each of the 2,863 solid boxes from four sides never ends inside one.
- Examining all 983 examinable NPCs: 290 boxes give their item and lose their lid, and 484
  crystals warp. The 20 town boxes (ids 180–184) are outside the random treasure system.

### Field props are occluders

The 27 field props (`dXXXX.bin`, `t0101.bin`; the `_stgalphaname` list, loaded via stage record
`+64`) have only untextured or solid alpha-only materials, with black diffuse, white ambient and
depth write. They are occlusion geometry over the painting's pillars and door frames. The engine
draws them depth-only (colour writes off) before the characters.

Checked by capture:
- The player is 63–69% hidden behind door frames in d0305/d0202, and 0% hidden in the open.
- No prop pixels are left visible.

Property bits: `glColorMask(bit 2, bit 2, bit 2, bit 1)`, bit 3 depth test, bit 4 depth write,
bit 5 a hint, bit 7 polygon offset, bit 8 two-sided lighting. Bit 10 routes the draw through
`_mceAppearance_switchDest` (appearance variants, used by most effects).

### Messages (header +0x28 / +0x2c)

Each area file holds two text sections: +0x28 Japanese and +0x2c English. The language index is
at +0x31dd0.

Lookup for `EvtMsg id`:
- **id < 51000:** m = id − 1000. The section is a table of u32 block offsets (from the file base;
  it ends where the first block starts). Block m / 300 is a table of 300 u32 message offsets;
  take entry m % 300.
- **id ≥ 51000:** system texts from `sys.evt`, loaded once by `gameapp_init`. Its header u32[language]
  points to the same kind of block table, with blocks of 1000 entries (m = id − 51000).

Text is UTF-16LE, terminated by 0. Control codes, from `PrcMsg`'s jump table (codes 0–30):

| Code | Meaning |
|---|---|
| 0x15 n | name plate: the speaker's name is system message 58028 + n (`font_getfont_utf16` block 7, entry n + 28; n = the character's jNNN number) |
| 0x1c s | speaking side: 1 = the left-hand portraits (0x1000 slots) speak and the right-hand ones dim to 128/255 (slot +0x28/+0x2a), 2 = the reverse |
| 0x01, 0x03, 0x16, 0x1a, 0x1b | one parameter; display state |
| 0x02 | one parameter: insert the value of var(param) |
| 0x1e n | voice clip n of `voiceevnt.bin`, numbered across all areas (j001_d04 uses 1–35, j001_d05 from 36, j001_t01 from 512, …); 65535 = none. Voiced builds only; 1.0.1 skips it (one parameter) |
| 0x10, 0x11 | line break |
| 0x12 | wait for a tap (new page) |
| 0x00, 0x13, 0x14 | end |
| other codes below 0x20 | display state only |

Typical messages start `1c 01 15 07`: the left-hand speaker, name plate 7.

Checked over 12,241 English messages, structure only:
- Median line 23 characters (95% within 48), no control codes left over.
- `crtool events`: scripts show 7,068 messages on a new game. 6,077 decode (8,583 pages, at most 9
  lines per page) and all 916 system messages decode from `sys.evt`.

### Playing scripts over time

`EventRunner` runs a script a frame (20 Hz) at a time and waits where the original waits:
- **`Wait n`:** n frames. 0xFFFF/0xFFFC wait for the screen fade, 0xFFFD + npc for an NPC's
  animation, and 0xFFFE (loading) doesn't wait.
- **`Msg`:** waits until the dialogue box closes.
- **Taps:** `touch_up_chk` loops wait for the action key.

It asks the host to:
- show messages and the `GetItem` window (it closes when the script raises flag 0 after the tap);
- play NPC actions (`Anim`, `Anim2`) and fade NPCs (`NpcFade`, `mdl_fadein`/`fadeout`);
- show and hide NPCs (`Born` / `Kill`);
- fade the screen (`Screen u16, u16 grey, u8 from, u8 to, u8 frames`);
- warp. The script continues on the new field unless the area changed.

Self-test, frame by frame:
- **Chest:** the lid opens at frame 0, the item window appears at 20 and waits for the tap, then
  the lid fades out and is removed at 73.
- **Exit:** fade out over 20 frames, warp at frame 20, fade in finished at 42.

### Cutscene frames (section 6)

Section 6 is a table of u32 file offsets, one per scene; `FrmSet n` selects scene n (from 1).
A scene:

| Field | Meaning |
|---|---|
| u32 | actor count; ≥ 10000 means count + 10000 with 6 words per actor, else 3 |
| u32 | record count |
| u32 × actors | NPC ids (0 = the party leader) |
| records | u32 frame, then per actor: position x, y, z × 100, and (6 words) rotation x, y, z in degrees × 100 |

The selected scene and record are global (+0x1dfc, +0x22cc), so the script that sets a scene up
and the ones that play it can differ (t01's script 0 places the hall's actors for the scenes its
NPC scripts play).

| Command | Effect |
|---|---|
| `FrmSet n` | scene n, its first record |
| `FrmJmp f` | the record with frame f, else the nearer neighbour, else the last |
| `FrmSetc2 npc, flag` | place the actor from the current record (position, Y rotation), y snapped to the floor (`get_floor`) |
| `FrmSetc npc, dir` | place from the current record; `dir` −4499…999 = degrees, 1000 keep the facing, 2000 face the way it moved, else 0 |
| `FrmAdjc npc, f, n, dir` | walk to record f's position over n frames in a straight line (`npc_autoctl_set_fream`), facing per `dir` |
| `FrmAdjc2 npc, f, n, mode` | the same; mode 0 position, 1 position and rotation, 2 position including y |
| `SetDir a, b` | a turns to face b |
| `Wait 0xFFFD, npc` | wait until the actor's move ends (`npc_autoctl_chk`; animations don't count) |

During a move y follows the floor each frame (unless mode 2). `Kill 0` hides the party leader
for a scene and `Born 0` shows it again.

| Command | Effect |
|---|---|
| `LoadAct npc, a, slot` | load action file a (table of 205 `.mca` names at 0xd4a00, `kActionFile`) into the actor's slot (`read_act_set`); a negative npc picks the other loader. A model's own actions fill slots 0… (stand, walk, run); event actions go in 4 and up |
| `UnloadAct npc, slot` | free the slot |
| `Anim npc, slot, a, b` | `mdl_set_act_motion_loop`: play the slot's action, then loop frames a..b; a = −1 plays it once and holds the last frame (chest lids), a = b = 0 loops it whole. The action already playing isn't restarted. `Anim2` has the same first four operands |
| `NpcMdlRead npc` | load an NPC from the NPC table even if the room's list lacks it (e.g. 501 = `t0152bt.bin`, the 3D hall the opening is filmed in): `__EvtNpcMdlRead` puts it at the origin, facing 0, scale from row +0x26, hidden (+0x8c bit 0) when its create flags fail or its id is 400 or more - a `Born` shows it. A negative npc re-reads the model of one already there, at its list entry's place |
| `Setc npc, x, y, z, dir` | place at world coordinates (shorts); `dir` as for `FrmSetc` |
| `CameraFree` / `CameraPlayer` | a script drives the camera / the stage camera again (`SetFreeCamera`, `SetPlayerCamera`) |
| `FrmSetCam id` | camera cut: the eye is actor `id`'s position in the current record, the target the next actor's (ids 1001…) |
| `FrmAdjCam id, n` | move to that shot over n frames |
| `FrmPanCam dx, dy, dz, n` | shift the camera over n frames |

`FrmSetCam` moves the 3D camera only in rooms filmed in 3D: maps 13, 29, 46, 88 and 113,
maps 63–87, or while +0x2c0 bit 6 is set. Elsewhere it scrolls the painted background
(`SetScroll`, `SetScroll2D`). Room 113 (`t0150`, j001_t01) has no painting and a zero
camera: the opening (event 201, script 0's gosub 0x5f98) is shot there.

### 2D images (`Tex2d`)

`Tex2d slot, flags [, image] [, 3 shorts per flag 0x4 / 0x20 / 0x8]` drives 16 image slots
(+0xbd10 + slot × 0x70). `image` indexes a table of 254 texture names in the binary (0xd3da0,
`kTex2dImage`): logos and title cards, the character portraits `jNNN_LNN_hd.mct` (512×512, one
per expression), item and background art.

| Flag | Effect |
|---|---|
| 0x0001 | load `image` into the slot (`read_img_set`) |
| 0x0002 | free the slot (+0xbd0c = −1: `tex2d_img_prc` deletes it once its fade and slide have run) |
| 0x0004 | fade: 3 shorts (from, to, frames), alpha 0..255 (+0x20 / +0x22, frame +0x24 of +0x26) |
| 0x0020 | grey level the same way (+0x28..+0x2e): the picture's colour, 255 = as drawn |
| 0x0008 | slide: 3 shorts (dx, dy, frames): from (x + dx, y + dy) back to (x, y); frames < 0 moves it there at once and it stays |
| 0x1000 / 0x2000 / 0x4000 | place it left (x −32, y 0), right (512, 0, drawn mirrored) or centre (176, 96), in 1024×768 screen pixels |
| 0x0400 / 0x0800 | show / hide: fade between 0 and 255 while sliding 32 px from / towards its side at 3 px a frame |
| 0x8000 | a full-screen card: a 1024-wide sheet's 1024 × 768 picture at tex2d (−32, −64), anything else 480 × 320 at (x, y) - 2.2.0's and the Vita's sheets hold a 960 × 640 picture (the Vita, 0x8101_22a2, always 480 × 320) |

Every one but 0x0001 / 0x0002 turns the slot on (+0xbd0d): a load alone shows nothing. The title
script loads its card (`Tex2d 0, 0x8001, 3`) and fades it in (`Tex2d 0, 0x0004, 0, 255, 20`);
the logos before it are the same, and Start fades it out and frees it (`0x0006, 255, 0, 10`).

A dialogue loads its speakers' portraits first (`Tex2d 2, 0x0001, 16`, …), shows the one who
speaks (`Tex2d 3, 0x1400`), and hides them (`0x1800`, `0x2800`) before `Close`.

## Event scripts

Bytecode run by `PrcEvnt` (0x7ab60): one opcode byte, then `EvtPrcTbl[op]` (0xcfac0, 61
entries) parses its own operands. Handlers return −2 to continue, anything else to yield a
frame. The context lives at +0x31e24:

| Offset | Field |
|---|---|
| +0x488 | pc |
| +0x200 | argument stack (u32 × 34) |
| +0x288 | frame bases |
| +0x490 | sp |
| +0x492 | frame index |
| +0x496 | "next Gosub opens a frame" |
| +0x498 | result |
| +0x4a2 | call depth (stack at +0, 64 deep) |
| +0x4a4 | loop depth |
| +0x4a6 | event number |

Opcodes and operand sizes (all verified by decoding every script: `python3 -I tools/evt.py --check`
gives 27,010/27,010 entries with no misaligned jump targets):

| op | name | operands |
|---|---|---|
| 0 | Return | — (pops a Gosub, or ends the event) |
| 1 | Gosub | u32 address |
| 2 | Abort | — |
| 3, 4 | Open (window) | u8 style, u16 lines (see below) |
| 5, 6 | Close | — |
| 7, 8 | Msg | u16 message |
| 9 | Set | expression, target |
| 10 | Goto | u32 |
| 11 | Break | u32 |
| 12 | Ifn | expression, u32 (jump if zero) |
| 13 | Rept | expression, u32 end (loop head; jump to end and pop if zero) |
| 14 | Wait | u16 frames; 0xFFFD + u16 waits for an NPC; 0xFFFF / 0xFFFE / 0xFFFC wait for fades / loading |
| 15 | Call | u16 native function |
| 16, 58 | Screen (fade) | 7 bytes |
| 17, 59 | Warp, Warp2 | 6 × u16 |
| 18 | Push | expression (argument) |
| 19, 20 | PushBP, PopBP | — |
| 21 | Util (menus) | u8 |
| 22, 23, 24 | Bgm, Eff, Voice | 4 bytes |
| 25 | Tex2d | u16 slot, u16 flags, then +2 if bit 0, +6 each for bits 2, 5, 3 |
| 26, 27 | Born, Kill (show / hide an NPC until the room reloads; 0 = the player) | u16 |
| 28 | Setc | 10 bytes |
| 29, 30, 31 | Anim, Autoadj, Mapscroll | 8 bytes |
| 32 | Battle | 4 bytes |
| 33, 34, 37, 48, 51 | FrmSet, FrmJmp, FrmSetCam, FrmStopEff, NpcMdlRead | u16 |
| 35, 39, 43, 52, 54 | FrmAdjc, FrmPanCam, Quake, FrmAdjc2, SetScale | 8 bytes |
| 36, 38, 40, 45, 50, 53, 57 | FrmSetc, FrmAdjCam, FrmFaceTo, UnloadAct, FrmStopEff2, FrmSetc2, SetDir | 4 bytes |
| 41, 42 | CameraFree, CameraPlayer | — |
| 44, 47, 56 | LoadAct, FrmPlayEff, FrmPlayEff2 | 6 bytes |
| 46 | NpcFade | 5 bytes |
| 49 | Anim2 | 12 bytes |
| 55 | AdjScale | 10 bytes |
| 60 | AAAA | 15 bytes |

**Open's windows** (`__EvtOpen`, `_SetWin`, `_PutWin`, `_PutMsg`; the same in 2.2.0). The style
byte's first bit of 2 / 4 / 8 / 0x10 / 0x20 / 0x40 picks rect 0 / 2 / 4 / 6 / 8 / 10 (else 0),
+1 when the lines are 1; the rects (0xd3d30, 4 shorts: x, y, -, the text's y below it; tex2d
units, logical = 2 × + (32, 64)) are (0, 176, 60) ×2, (0, 8, 8), (0, 224, 8), (0, 16, 16), (0, 108,
16), (0, 0, 44), -, (0, 0, 264), -, (0, 8, 16), (0, 216, 16). `PutWin` by style: 0x210 or 0x80 no
window; 0x20 a black band x −16..512, y 252..352; 4 a light band x..480, y..+0x58; 8 a dark band
(alpha 0xc0) x−16..496, faded in over y..+8, solid to +0x60, faded out by +0x68; 0x40 window_hd
(0, 704, 480, 96) at (x, y) - the grey panel; otherwise (2) window_hd (512, 120, 512, 144) at
(x − 16, y + 32) - the parchment. `PutMsg`: lines 24 apart from y + text y (+18 for style 2),
x 8 for style 8 in English (else 36); style 0x10 draws each line in black at the 8 one-pixel
offsets first, 0x20 uses font 0x10; the name at (24, 222) (0x20: (16, 236)); the "next" mark at
x 452, y 328 (style 4: 100; 8: y + 84; 0x40: none). Message code 0x17 (no operand) sets the line's
+0x10e: `CmdFont` centres a line about tex2d x 240 (240 − width / 2) when the window's style has
no 0x10 and the line is marked, or the style has 0x10 and it isn't; 0x16 v sets the text colour
(1 black, 0 white); 0x18 takes two digit characters (a number, d1 × 10 + d2: the typing's
frames per character less one, while typing; neither release's text uses it). **Typing**
(`PrcMsg`): a page comes out a character a frame (+0x334a4 counts to the speed +0x334a0, 0 from
start); +0x33433 set means typing - a tap (or the skip circle at tex2d (476, −16) r 20) clears it
and the rest of the page comes out at once; 0x10 starts a page with it set. 0x1a n stops the
typing n frames there (+0x334a8). The ends: 0 waits for a tap (sets flag 0xfeb); 0x14 closes
without one (+0x3342d = 8) - so 0x1a n then 0x14 closes n frames after the last character; 0x13
d takes a window mode from the digit. 1.0.1's English has 57 `1a 01 14` (a pause, no text), one
speech that closes itself (j005_d05's 5203: five 60-frame pauses; 2.2.0 retimes them for the
voice: 60 60 80 60 40 70 60) and j007_d05's 4601 (two characters, closing a frame later). In the
video a centred line types from where the whole line starts. Scripts use 2 (speech), 64 with one line
(examined things, chests), 8 (tutorials), 16 and 32 (scenes).

**Loops.** `Rept` pushes `{its own address, end}` on a loop stack (ctx +0x100, depth at +0x4a4,
32 deep) on its first execution. Before every opcode fetch, `PrcEvnt` checks whether the PC has
reached the innermost loop's end and, if so, jumps back to that `Rept`. A false condition jumps to
the end and pops; `Break` jumps and pops too. There is no backward `Goto`.

**Expressions** (`GetEval`, table 0xd3a20) are prefix trees, one operator byte each:

- 0–8: == != > >= < <= && || ^, each with two operands. Every binary operator computes
  `second OP first`: `__GT` is `cmp first, second; movlt`, and `__SUB` is `second − first`, as are
  ÷ and %. `tools/evt.py` prints them in that order.
- 9: ! (one operand)
- 10–14: + − × ÷ %, each with two operands
- 16: immediate i32
- 17: result
- 18: rand(n)
- 19: var(e). Negative indexes reach other globals, e.g. the start position; below −198, +317.
- 20: flag(e). −1 / −2 name the running NPC's show / hide flag.
- 21: label (u32)
- 22: arg (u8, relative to the frame base)
- 33 gold, 34 name, 42 gds; 38: item(e)

**Assignment** (`SetEval`) evaluates the value first, then a target from table 0xd3ad0: result,
var, flag, arg, gold, name, item or gds. `EvtStart` sets var 62 = area and var 63 = map. Flags
are 4096 bits at +0x2f950 and vars are i32 at +0x2fb50.

Compiled patterns: `PushBP; Push a; Push b; Gosub f; PopBP` calls a function with arguments,
which it reads as `arg(0)`, `arg(1)`. An exit script typically sets "visited" flags, then fades
out, unloads actors, calls `Warp 0xFFFD`, and fades in after the reload.

**Scripts the game starts itself.** When an area's event file loads, `init_evnt` runs script 0,
the area's entry script: it picks the music and screen state, shows or hides NPCs by story
flags and the map number (var 63), and stages first-visit scenes. On other room changes
`fldread_prc` runs entry 5 of section 1 (`EvrStart`), which repeats the per-room part. The other
section 1 entries are run after battles and item use (2), camping (3) and shops (4). Room
changes inside a running script (an exit's `Warp`) run them after it finishes.

A new game starts in the Ark's hall (`j001_t01`, map 97) with flag 4089 set by the title's
new-game path. Script 0 then plays the opening (gosub 0x5f98: room 113, then back to the hall),
starts track 16 and places the hall's actors from scene 20; talking to them plays the
introductions. With 4089 clear (a loaded game), script 0 marks the opening as seen (flag 1091)
instead.

**Natives (`Call n`, table at 0xd2b74, 12 bytes per entry: init, process, draw):** 2001
`Uty_touch_up_chk` (var 0 = tapped), 2002 `init_tex2d`, 2003 puzzle start, 2004 / 2010
`mdl_fadeout` / `mdl_fadein`, 2005 `GetItem` window, 2006 `break`, 2007 `DrawExclamationMark`,
2008 `DrawController` (tutorial art), 2009 `ScnEffect_Init`, 2011–2013 `AddItem` / `DelItem` /
`ChkItem`, 2014 warp crystal camera, 2015 `FireWall`, 2016 `TexDisp`, 2017 `Mode`, 2018
`HpMpRecovery`, 2019 `mdl_set_alpha`, 2020 `ScnEffectRead`, 2021 `LoadData`, 2022 `LoadPrty`,
2023 `eff3d_set_fld`, 2024 `GetBgmNo`, 2025 `EndMenu`, 2026 `BgmPlayFromVarNo`, 2027
`GetPartyNo`, 2028 `NowLoading`, 2029 `ChkLogo_flg`, 2030 / 2031 battle parameter rises, 2032
`SetOpenedTresureBox`, 2033 `TutoTexDisp`, 2034 `LookatRect`, 2035 battle first animation.
Arguments are `GetXmpStkData(k)`: k = 0 the last pushed, i.e. the frame's argument count − 1 −
k (`ScnEffect_Init`: 0 on / off, 1 the effect; `eff3d_set_fld`: 0 the effect, 1 the NPC, 2 the
height). 2016 `TexDisp` (11): slot, x, y, u, v, w, h, fade-in, hold, fade-out frames, ? - state 0
alpha + 255 / fade-in a frame, 1 until the counter passes hold, 2 alpha 255 / fade-out × (fade-out
− counter) until the counter reaches fade-in, 3 flag 0 cleared; drawn (`tex2d_add_sca` on slot +
0x10) as a strip when w × h is 480 × 96 - (2u, 2v) of the sheet, 1024 × 192 at (2x − 32, 320 − h)
of the HD frame, the screen's (2x, 288) - else the whole picture over the screen. The dungeons'
area names: Tex2d 0x8001 loads the sheet (EvrStart 0 of areas 2, 3, 9, 10, 16, 22, 23: rows
0 / 96 / 192 / 288), fades 10 / 20 / 10. 2033 `TutoTexDisp` (12): slot, pages, (row, frames) ×
4, x, y; `MsgUty_TexDispInit(var 1)` reads telepo / psyco / tough / pyro_01 / _02_hd (2048 wide),
`MsgUty_TexDispProc`: each page turn of the message (`PrcMsg` sets flag 4075) the next page
(page −1 before the first), done after the last or when flag 3 clears; frames 384 × 256 (frame ×
384, row × 256), stepping every 20 frames, the last held 40; drawn while flag 3 is set at (2x, 2y)
of the HD frame. 2007 `DrawExclamationMark`: flag 0 set; until it clears, black 0xc0 over the
screen and the "!" (468, 440, 44 × 48) at tex2d (220, 180). Tex2d flag 0x8000 only sizes a slot
(1024 × 768 at (−32, −64), else 480 × 320); a slot is drawn once 0x200 (at once), 0x400 (show) or
a 0x4 / 0x8 / 0x20 animation turned it on - a 0x8001 load alone shows nothing. 2018: `pcparam_hpmp_recovery` - the
current pair's two (G+0x30548 the pair 0..3; `pcparam_hpmp_all_recovery` all eight). 2024: var
[arg 0] = G+0x35530, the music last asked for. 2025: arg 0 = 2 ends the shop
(`SetShopBgmNoRecovery`, `__mc_SetMenuStat(7, 4)`). 2026: `_bgmgo(var[arg 0])`. 2027: var[arg 0] =
the pair. 2029 `ChkLogo_flg`: var 0 still 0 takes the run's "logos played" byte, which is then
set. 2008 `DrawController`: flag 0 set; until it clears the cross key (1864, 1584, 144 × 144 of
window_hd's pixels) at (64, 384) and its centre (1864, 1448, 96 × 96) at (88, 408) of the HD
frame. 2002 `init_tex2d` (texture setup) and 2021 `LoadData` (the continue / save data into the
work area) do nothing visible. 2028: flag 4086 set `NowLoadStart`, else `NowLoadEnd` - `NowLoadProc` draws (820 + 24
(n % 8), 96, 24 × 24) of the window sheet at tex2d (464, 320), n + 1 a frame. 2030 / 2031
(`Uty_Btl_ParamUp01` / `02`): bytes +0x13b..+0x13e / +0x8b..+0x8e of the battle work = 6 -
fighter 1 / 0's attack, defense, speed and luck ups (statuses 2..5, 0xb0 a fighter) for 6 rounds;
called by the story scripts 201 / 301 of areas 12 and 24 (the port keeps them for the next battle
when none is on). 2035
(`Uty_Battle_first_anm_Init`): `btl_anm_load(arg)`. The shop (`ShopSet`) keeps var 0 and plays
track 0x13; `ShopClr` plays the kept track back unless `SetShopBgmNoRecovery` was called.
`crtool natives` runs every script once and lists the calls met (`CR_NATIVE_ARGS=2016,...` prints
their arguments; `CR_TEX2D_FLAGS=1` the Tex2d flags). 2035 (a battle animation preload) is a
no-op: the port loads animations as they're needed.

**Menus (`Util n`):** 0 title, 1 world choice, 2 shop, 3 yes/no, 4 start, 5 / 6 save and load, 7 staff roll (var 0), 8 last
battle.
- **Yes/no** (`_menuu_Event*`, `ComUty_YesNoChoiceProc`): the left button returns 2, the right 1;
  on close 2 sets flag var(0) and 1 clears it, so Yes sets the flag. `__menuu_EventPut`:
  `ComUty_YesNoChoiceDraw(window 0)` - `__ComUty_CmdBottom` plates (icon / word 0 Yes, 1 No) at
  (336, 104) and (336, 160) of window 0, which slides in from x + 360; the question stays in the
  message window (the scripts' `Open 64, 1` grey panel).
- **Plates** (`ComUty_ComPlate_*`, drawn by `MuScrList_Draw` by list type): `_Lvl` / `_Wld`
  picture 8 (9 chosen), icon 0x3d at (12, 8), the text at (56, 12); `_Equip_GenePlate` the
  plate's six entries as pictures 10 / 11 at (251 − 14 × 6 + 14k, 30) (the unknown ones first,
  10) or, all known, picture 96 (MASTER) at (176, 24); the name in colour 3 when worn, 2 when none
  of it is known (`GetSkillNumFromPlate`). The list also draws its "more" marks (pictures 0x8c at
  (280, 112) / 0x8d below), a scroll gauge (`ComUty_GuageBarDraw`), Back and a HELP window.
- **World choice** (`_menuu_WldChoice*`): a list of gates, then a level and a yes/no. On close
  `_menuu_WldChoiceClr` sets flag 0 if cancelled; otherwise var 0 = gate (0 = 1st), var 1 = level,
  var 2 = the party (route), flag 1 = the second choice (cleared for gates 4 and 5), and
  G+0x30540[pair] = the level. The Ark's script returns when flag 0 is set, else warps: gate 0 →
  area 2 (d01) map 0, gate 1 → area 3 map 14, …
  Set: the pair's gates in its order (0xca7d0, 6 bytes a pair: 0 1 2 3 4 5 / 1 2 0 3 4 5 / 2 0 1
  3 4 5 / 3 2 1 0 4 5), each offered when flag 0xca810[pair] + gate is set (1076 / 1476 / 1876 /
  2276 + 0..4; the sixth, Original Door, 0xca820: 1104 / 1504 / 1904 / 2304; none set: the first
  alone); the level bands are 2 + the pair's eight flags at 0xca830 set (pair 0: 1123 1143 1163
  1183 31 31 32 32; then +400 a pair, and 33..38); textures 0x11.. = MAP_01..06.mct. Windows (all
  at (0, 0), sliding in): 0 picture 0 at (8, 4) (the title art), 1 the focused gate's MAP picture
  (sheet 1 picture 1, 256 × 256) at (0, 64), 2 picture 15 at (0, 56) under block 7 entry 0
  ("World Select") at (12, 40), 3 picture 16 at (24, 72) under entry 1 (">Level Select") at (40,
  58), 6 / 7 the lists (`MuScrList_SetIdInfo(0, 6, 7, 2, 0, 1)`: items `ItemRegist(gate + 1)`, the
  names block 7 entry gate + 2; `(1, 7, 7, 8, 0, 1)`: bands, entry n + 8), 8 Yes / No, 9 / 10 the
  message board (y 220): block 7 entries gate + 2, band + 8, 0x12 ("Is this okay?"), or for the
  sixth gate its name and 0x12, or block 6 entry 0x40 ("Skip the first half?"). A gate plate
  (`ComUty_ComPlate_Wld`): picture 8 (9 when chosen), icon 0x3d (sheet 0 picture 14's grid: 32 ×
  32 from (0, 248), 16 a row) at (12, 8), the name at (56, 12), and for gates 0..4 four pips at
  (195 + 14k, 30): picture 11 if flag 0xc9bb0[gate][k][pair] is set (gate 0: 1122 1123 1052
  1053 for pair 0), else 10. States (0x2e044): 0 set up, 1 the gates (Back: everything off, 9),
  2 → 6, 3 wait, 4 "skip?" (gates 0..3 with flag 0xca7f0[pair][gate] set - 1122 1142 1162 1182
  for pair 0 - Yes = flag 1), 6 the bands (Back: to 1), 7 wait, 8 "okay?" (No: back to the bands
  or, for the sixth gate, the gates; Yes: 9), 9 wait, 10 `__mc_SetMenuStat(4, 4)`.

Event scripts chain: when the top-level script returns, `EvtReturn` starts the next
queued event (ctx +0x494 count, +0x308 ids).

## Battles

### Random encounters (`fldencount_*`, area file section +0x14)

The section is a u32 per map of the area (from its first map) → the map's entry; the entries
follow the table, so it ends where the first entry starts. An entry:

```
+0  u16 stage        0xFFFF = no encounters; low byte = kBattleStageFile index (_btlflrname)
+2  s16 distance     units walked between checks, / 10
+4  s16 rate         % chance at a check
+6  u16
+8  12-byte groups, up to 128, ending with group 0xFFFF:
    +0 u16 group     1-based enemy group (sys.evt)
    +2 s16 level     only when the pair's average level is at least this
    +4 s16           kept for the battle (G+0x354c2)
    +6 s16 weight    relative chance among the eligible groups
    +8 s16 first     % the party strikes first (50 with gene 171 on either member)
   +10 s16 ambush    % the enemy strikes first (never with gene 172)
```

`fldencount_init` (every room) zeroes the walked distance (float G+0x3551c) and sets the chance
(G+0x35524) to the rate. `FieldPCCtrl::checkHit` passes each frame's movement length to
`fldencount_walk`, which adds it unless flag 4076 or `unencountflg` (save +0xc1b + route, the
pair's own; skill 170's field button) is set. With skill 174 worn by either member of the pair
it also adds the length to G+0x35520; at 10 or more (checked once a step) the pair's bank
(save +0xac0 + 4 × route, at most 100000) gains 1 and 10 comes off. `btl_main` 0x44b (a win)
adds the bank to the battle's EXP (+0x9b0) and empties it, before the Paradise Apples double
it. `CRView prc` calls
`fldencount_encount` every frame while G+0x2c0 bit 6 is set (not with bits 14, 0 or 5): every
distance × 10 units it draws `random(100)`; at most the chance a battle starts and the chance
resets, else it rises by 5. The group is drawn by weight; the strike by the two rolls. It stores
group, stage, the +4 value and the strike at G+0x354c0/+0x354c5/+0x354c2/+0x354c4, sets G+0x2c0
bit 7 and vars 58 = 999 (battle music: default), 59 = 10 (victory tune), 60 = 999.
`_get_random(n)` is uniform 0..n-1 from a Mersenne Twister.

Script battles (`Battle`, opcode 32, `__EvtBattle`): u16 group, u8 stage, u8 flags (G+0x354c6);
extra = -1, strike 0, then the same bit 7, and it returns −1 (`PrcEvnt` loops while an op returns
−2; −1 ends the frame, ≥ 0 repeats the op next frame), so the script stops right after it. The
frame loop (`CRView::prc`) tears the field down: G+0x2c0 bit 5 (battle) for 6, `SetFade(white,
255, 255)`; `btl_main` 200 fades in from white over 1000 ms. If a script is running then (G+0x2c0
bit 0), `btl_main` sets G+0xa4e = 1 - an **event battle** - and the script is kept. In a battle
`PrcEvnt` runs only while flag 0xfff is set, and checks after every op: once it's clear the
script yields (an event battle's) or ends (others'). The battle's states (400, 600, 800, 8000)
wait while 0xfff is set; `__menub_karaok_on` (G+0x2f0b4) gives the screen to the talk.

- **Event battles' moments** (`_chk_karaok_*`, G+0xc18 the route, G+0x354c0 the group): each
  sets 0xfff, and the event script goes on from its Battle. `frist` (300 → 400): route 2 group
  0x156, any route 0x164..0x167. `between` (state 899, a round over, everyone standing; +0x1a
  the round): route 0 0x104 round 3, or after it with +0xa50 and flag 0x4d0 clear; 0x173 3,
  0xad 2, 0x105 3, 0x154 3, 0x158 2, 0x160 / 0x170 enemy 2 at half its HP or less (flags 0x4d7 /
  0x4d9 clear); route 1 0x118 3, 0x119 3, 0x159 2, 0x161 / 0x170 half (0x665 / 0x667); route 2
  0x178 2, 300 3, 0x176 3, 0x156 a fifth (0x7f3), 0x17a 3 (0x7f4), 0x15e 5 (0x7f5), 0x162 / 0x170
  half (0x7f6 / 0x7f8); route 3 0x140 3, 0x177 1, 0x15b 2, 0x163 / 0x170 half (0x985 / 0x987);
  any route 0x164..0x167 half (0x4db, 0x4dd, 0x4df, 0x4e1). `end` (btlord_chk 2, the enemies
  down → state 8000, waiting for the talk before the victory): route 0 0x154 0x158 0x160, route
  1 0x155 0x11b 0x159 0x161, route 2 0x162, route 3 0x157 0x15b 0x163. `dmg_hp`
  (`btlcalc_dmg_hp`, every hit and poison): while the group's flag is clear no damage takes a
  fighter below 1 HP - route 0 0x104/0x4d0 0x173/0x4d1 0xad/0x4a9 0x105/0x4d2 0x154/0x4d3
  0x158/0x4d5 0x160/0x4d7 0x170/0x4d9, route 1 0x118/0x65f 0x119/0x660 0x159/0x663 0x161/0x665
  0x170/0x667, route 2 0x178/0x7ef 300/0x7f0 0x176/0x7f1 0x156/0x7f3 0x17a/0x7f4 0x15e/0x7f5
  0x162/0x7f6 0x170/0x7f8, route 3 0x140/0x97f 0x177/0x980 0x15b/0x983 0x163/0x985 0x170/0x987,
  any 0x164/0x4db 0x165/0x4dd 0x166/0x4df 0x167/0x4e1. `btlcalc_status_prc` (round end): route 0
  0x104's enemies heal whole while 0x4d0 and +0xa50 are clear; route 2 0x17a / 0x15e's enemy 2
  while 0x7f4 / 0x7f5 are. +0xa50 is set by `btlcalc_0404` when a physical hit of 1 or more
  from action 0x12 (route 0's pair attack) lands after round 3. E.g. j001_d05 script 201: Battle
  260 (0x104), then two talks (flags 1231 = 0x4cf and 1232 = 0x4d0 set, each ending `Set
  flag(4095) = 0`).
- **Tutorials** (non-event battles; not while a script runs or 0xfff is set; once each, the
  flag recording it): `btl_main` 400 after the opening sets 0xfff, var 1 = 0xff1 and runs
  EvrStart 2; 0x5dc (the Break's effect done) 0xff2; 700 (an action done) 0xff3 if +0xa6 (a
  gene gave its users its element, 0406), else 0xff4 if +0xa7 (a status took hold: 0406's
  poison, 0302 05..12), else 0xff5 if +0xa8 (0506 taught a gene); `__menub_mainPrc` 0xfa8
  on Genes (not silenced), 0xfa9 on Defend. EvrStart 2 compares var 1 with each key, sets that
  flag, shows its pages (`Open 8, 2`, a `TutoTexDisp` picture: `Gosub` with the row in arg 0 -
  `MsgUty_TexDispInit`'s table at 0xc9500, 24 bytes each (name, width, height): 0..7 the puzzles'
  `telepo` / `psyco` / `tough` / `pyro`, 8..10 `battle_01..03_hd`, 11 `gene_01_hd`, 12
  `puzzle_01_hd`, 13 `battle_04_hd`) and ends `Set flag(4095) = 0`.
- **The Break changing hands** (`btlord_chk` after an action: +0x18 bit 10 set - a side lost it
  - returns −1, round-over saved at +0x40): `btlbreak_effect_init` (sound 0x47), then
  `btlbreak_effect_prc` each frame (state 0x5dc): +0x98 0 in (t += 18 to 90: slide 480 sin t,
  alpha +64 to 152, labels +96), 10 held 30 frames (at 15: +0x14 = bit 11 ? 4 : −4 - the other
  side takes it - bits 10 / 11 cleared, sound 0x7a; at 30 sound 0x48, or 0x49 when the gauge is
  below 1), 0x3e6 out (slide 480 sin t + 480, alpha −64), 999 done → 600 / 800 and the 0xff2
  tutorial. Drawn in tex2d units: white (alpha) quads (−16, 128) (264, −32) (−16, 184) (360, −32)
  and (176, 352) (496, 172) (272, 352) (496, 224); window_hd (704, 328, 160 × 104) "Player" at
  (pos cos 30° + 114, −22 − pos sin 30°), (864, 328) "Enemy" at (250 − pos cos 30°, pos sin 30° +
  240), (512, 432, 512 × 72) "BREAK!!" at (slide − 496, 114), (512, 504, 512 × 32) "The battle
  situation changed" at (464 − slide, 186).
- **The calcs' messages** (`btl_add_msgque(fighter, 6, entry)`): a queue of five 8-byte places
  at G+0xa14 (fighter s16, font block, entry; count +0xa3c), one per fighter (a second for the
  same fighter is dropped). `__menub_mainPut` takes the first whenever the banner (+0x3c3e8) is
  free: a fighter < 0 is −item (var 9 = the item, Steal Item), else var 0 = its name
  (`btl_set_AnnounceTxt_Name`: block 2 at fighter +0xaa); `font_set_utf16(0xb, 6, entry, 0x3c)`
  - 60 frames (the action's name is 0x28, 40). `btl_main` starts no next action while the
  banner is up or the queue isn't empty. Entries: 3 a command cancelled (57003), 4 silenced, 5
  an item stolen, 6 poisoned, 0x19..0x1c attack / defense / speed / luck up, 0x1d the barrier,
  0x1e endure, 0x24 invincible, 0x32 + element an element given (`zokusei_chr1`). Shown at
  once instead (no queue): 57074 the poison didn't take (row +0x2e), 57075 no silence (row
  +0x30), 57037 / 57038 nothing to steal / failed.
- **Back on the field** (`fldread_prc` 9999): after an event battle the script goes on (G+0xa4e
  = 1 skips EvrStart 5); after others the field reloads and EvrStart 5 runs again.

In 1.0.1, 265 of 523 rooms have encounters: 112 groups of 48 monsters on 9 stages
(`crtool battle`).

### Battle data in `sys.evt`

`gameapp_init` loads `sys.evt` for good and points globals at its header's sections (offsets
from the file start); u32[0..7] are the languages' texts:

| Header | Global | Contents |
|---|---|---|
| +0x28 | +0x3bfbc | enemy groups: 3 × s16 monster ids (1-based; ≤ 0 empty), 516 groups |
| +0x2c | +0x3bfc0 | actions (attacks, genes, items): 24 bytes each, below (the voiced releases: 38) |
| +0x30 | +0x3bfc4 | monsters: 300 × 204 bytes |
| +0x34 | +0x3bfc8 | battle animations: u32 offset per battle model → 18-byte action slots, 13 for most models, 14 / 15 for the bosses (a model's run ends where the next one's starts; `btlanm_get_*` index them unchecked) |
| +0x38 | +0x3bfcc | items: 502 × 20 bytes (`GetItmPara`), below |
| +0x3c | +0x3bfd0 | battle cameras: 32 bytes each |

An item (from item 1; `GetItmPara(n)` = base + 20 (n - 1)):

```
+0  u32 price      Oz; selling pays price / 2 (GetSellPrice); 0 = not sold / unused
+4  s16 HP, MP, Attack, Defense, Speed, Luck   equipment bonuses (+4 .. +14)
+16 s8  icon       0..63: the 32 x 32 icon at (n % 16 × 32, 248 + n / 16 × 32) of window_hd
+17 u8  kind       1 / 2 consumables, 6 weapon, 7 armor, 8 jewel
+18 u8  ?
+19 u8  owners     bit c: character c can wear / sell it (0xFF everyone)
```

Shops (`__GetShopItemTbl`, `MuCom_ShopMakeBuyItemData`) stock, for the character c the board
shows (0..7) and the shop's level L (from the pair r): consumables from {item, from level} at 0xc86d0 (1, 21, 28 from 2; 2, 22, 29,
32 from 3; 33-35 from 4; 36-38 from 5; 30, 31 from 6) plus the seeds 3-6 once flag 39 is set;
weapons 201 + 28c.. and armor 215 + 28c.. (L of each); jewels 426.. (L - 1). The level
(`MuCom_GetShopBuyLevel`): 7 if flag {31, 33, 35, 37}[r]; else top = 7 if flag {1104, 1504,
1904, 2304}[r] else 6, less one for each of the progress flags {1074, 1474, 1874, 2274}[r],
-1, -2, -3 that is clear before one that is set (at most 4), at least 2. Selling lists owned
items of 1-60 / 201-424 (kind 6) / 201-424 (kind 7) / 425-438 with the character's owner bit. One
buys at most Oz / price and up to 99 of an item; skill 173 on either character takes 10% off.
Money is AppCtl +0x2f93c, capped at 9,999,999 (`MuCom_AddOz`); the scripts' `__GOLD`
expression and setter are stubs (0). `__ITEM n` is "owned?"; assigning v to `item[-1]` gives
item v, to `item[-2]` takes one away.

Characters (`_pcparam` at 0xd2cd0, 12 bytes each, Escher, Musiea, Eluca, Zhamo, Olgar, Vahti,
Ayuta, Mana): s16 HP, MP, Attack, Defense, Speed, Luck. `pcparam_max_*`: HP = base + 50 × level +
equipment + the save's extra points; the others base + 5 × level + equipment + extra + genes 175..186.
The extra points (member +0xc HP s32, +0x16 MP, +0x1c..+0x28 Attack..Luck) gather three things:
`pcparam_change_lv` - each level-up leaving level L ≥ 20 adds 10 L − 190 HP and L − 19 to the
other five (so +2100 HP / +210 at 40, +31600 / +3160 at 99) and 10 HP / 1 for each of skills
164..169 worn - capped at 50000 / 5000 after; and the seeds (`use_item`, one more while the
stat's extra is below 5000; `MuCom_GetUsedItemLimitNum` = 5000 − extra). 1.0.1's loop runs
`levels − 1` steps when a battle gives two or more levels (`do … while (step + 2 <= levels)`);
2.2.0's runs one a level (`while (--levels)`) - the port keeps 2.2.0's.
A new game (`prty_init`, `_prty_init_equip` 0xcb3f0, 14 bytes each): level 1, weapon 201 + 28c,
armor 215 + 28c, jewel 426 for Musiea, Eluca, Vahti and Mana. Experience (`_exp` 0xd2d30, `GetNextExp`):
u32 total for levels 1..100. Checked against the reference playthrough's shop board (Escher LV 17:
HP 842 + 850 + 4 = 1696, NEXT 21080 - 18078 = 3002).

The shop screen's pieces (all windows at 0, 0): Back is `ComUty_CmdBack` type 1 at (192, 288);
Yes / No (`ComUty_YesNoChoiceDraw`) at (336, 104 / 160); the message board (`ComUty_MsgBoardDraw`)
picture 12 at (0, 220), lines from (24, 234), 16 apart; the board (`ComUty_StateBoardDraw`) at
(12, 92): face 144 + n at (-8, -32), white name box (12, 0) 143 x 16, LV box (12, 32) 143 x 15 with
LV (82) at (50, 36) and the level in outlined digits (style 2) at (80, 33), HP / MP
(`__ComUty_FaceBoardHpMp`) at (-1, 56 / 76): label, current at +22, "/" at +82, max at +94, the
140-wide bar (`__ComUty_SubColorBar`, colours at 0xc8800: HP 0xdd0200 -> 0xffc373, MP 0x07af73 ->
0xffc373, experience 0x0284f4 -> 0xdccbff) at +16 / +9; NEXT at (-2, 96); EXP and the stats' label
boxes (83..87) at (12, 116 + 20k), values at 68 (EXP, 7 digits) / 80; the change for an item:
▲ / ▼ (93 / 94) at 158, + / - (89 / 92) and red / cyan small digits from 128 / 135 on the stat
rows (56, 76, 136, 156, 176, 196).

A monster (`read_btlcpl`, `btl_enemy_init_lv`):

```
+0  u32 HP         + level × 200 (strong growth) or × 30
+4  u32 exp        + level²
+8  u32 Oz         what it leaves, + level × 15 or × 10 (btlcalc_chk_dead adds it to the purse)
+12 s16 model      kBattleModelFile index (_btlcplname: j001-j008, m101..., m001a...)
+26 s16 size       1-5, the digit the formation code uses
+28 u16 growth     non-zero: the strong growth figures
+16 s16 resist     physical (+16), genes (+18), elements 1 / 2 / 3 (+20 / +22 / +24, element 0
                   counts as 1): 1 none, 2 half, 3 x1.5, 4 instant death
+32 s16 level      > 0 fixed; else L, the leader's level (the pair's average when it's the
                   route's own pair): strong growth uses L; the others a random level in the
                   route's progress band b (10b + 1 .. 10b + 10) kept within L - 5 .. L + 2
+38 s16 attack     + level × 12 or × 6
+40 s16 defence    + level × 10 or × 4
+42 s16 speed      + level × 8 or × 5
+44 s16 luck       + level × 9 or × 4
+62 u8  ?          copied to its queue entry (+0xb)
+70 s16 × 50       the AI's action lists, 5 each: 0 always (weight 100), 1 while the enemies
                   hold the Break / 2 otherwise (50), 3 at a quarter of the HP or less (100),
                   4 every fifth round (200); 5..9 the same when two enemies act together.
                   Actions 413..429 ("does nothing") weigh 10 wherever they are
```

Its name is system text block 2: message 53000 + id − 1 (`btl_set_AnnounceTxt_Name`).

**Formations.** The monsters' sizes as decimal digits (first = ones) form a code that is looked
up in `_btl_init_index` (48 s16); index / 3 picks a row of `_btlinitpos` (16 × 12 × {x, z},
stage units). Slots 0-2 are the enemies' places. The pair stands at row 0's first two places,
(508, 523) and (492, 523). Enemies face +z, the pair −z. Battle stages are centred on (500, 500).

**Action slots** (`btlanm_get_*`): +0/+2 loop start / end (0, 0 = the whole action), +4/+6 move
start / end, +8/+10 hit frames. Slot k is the package's built-in action k + 1 (named "k+1.n"):
monsters idle in slot 0 and attack in slot 8 (hit at 11); slot 0 is the pair's stance too, and
their slot 1 (a lead-in, then 53-112 looping) looks like the victory pose.

**Cameras** (`btlcamera_set`): s16 eye x, y, z, target x, y, z (10000-based values are relative
to an actor), then timing. A battle opens on camera formation − 2 (camera 0, for formation 2,
looks from (488, 12, 555) at (500, 4, 498)). `btl_prc`
projects with `mceCamera_setPerspectiveFov_op`: 0.0792 turns (28.5°), 4:3, near 9, far 2000.

**Music** (`btl_main`): var 58 when it is 28 or less, else track 8 (9 once the route's progress
byte passes 4); the victory tune is var 59; losing plays 0x1a.

### Battle rules (`btl_main`, `btlcalc_*`, `btlord_*`, `btlai_prc`, `btlbreak_set`)

The context keeps five 0xb0-byte fighters (0, 1 the pair, 2..4 the enemies): +0x58 HP, +0x5c max,
+0x64 / +0x66 MP, +0x6a attack, +0x6e defense, +0x72 speed, +0x76 luck, +0x88 element, +0x89..+0x93
statuses, +0x95 defending, +0x98 level, +0xa8 the formation's camera slot, +0x50 / +0x54 the
experience and Oz it gives. The pair's stats are the menus' (`btlpcparam_prty_init`): HP full at
every battle's start, MP from the save and back to it afterwards.

An action (`+0x2c`, 1-based):

```
+2  s16 power     +4 s16 MP     +6 s16 speed (added to the turn order)     +8 s16 calc (btlcalcTbl 0xd2c30)
+10 u8 icon       +11 s8 class: the animation slot (8-10 attacks, 11 genes, 12 items, 13/14 bosses)
+12 u8 scope      1 self, 2 / 4 / 6 one of own side / the other side / anyone, 3 / 5 / 7 all of them
+13 u8 side       1 self, 2 own, 3 the other, 4 any       +14 u8 the AI's pick: 1 random, 2 lowest HP, 3 highest, 4 / 5 the first / second
+15 u8 always hits   +16 s8 variance %   +17 u8 motion (1 runs up to the target)   +18 s8 element (sets the user's)
+22 u8 the element triangle applies
```

Actions 10..17 are the characters' attacks (character + 10; monsters use them too), 18..21 the
routes' pair attacks, 26 Defend, 100..125 the battle items 21..46, 201.. genes; the names are
system text block 3 (54000 + action − 1). Calcs (btlcalcTbl at 0xd2c30, power p):
0 physical (`btlcalc_0404`), 1 genes (0406), 2 p × 100 shared by the targets (0407), 3 p × 100
(0408), 4 p × 10 MP (0409), 5 p% of the target's HP (0410), 6 p% of its MP (0411), 7 Sacrifice:
p% of the users' HP above 1, which they lose (0412), 8 drain: p% of the attack sum, the users get
it back over the targets (0413), 9 down to 1 HP (0414), 10 takes p% of the purse (0415; the thief
keeps it at +0x9c until it falls), 11 the monster runs: 50 / 70 / 85 / 100% for its level less the
pair's < −49 / < 0 / < 50 / more, sure while the enemies hold the Break (0416, +0x93), 12 healing
(0417), 13 MP by the luck formula (bonus 20 / 5, enemies 25 / 5; 0418), 14 all HP and MP (0419), 15
/ 16 p% of the most HP / MP (0420 / 0421), 17 / 18 item HP / MP × 10 (0422 / 0423), 19 revive with
half (all, holding the Break) the HP (0424; actions 108 / 230 can target the fallen), 20 / 21
nothing, 22 cures poison (042700), 23 every status and the element (042701), 24 Analyze (0428: no
effect of its own), 25 the target takes the action's element (`zokusei_chr1`), 26..33 statuses
(+0x8a the magic barrier: the next gene whose action +23 is 0 is "Neutralized" and the barrier
goes; attack / defense / speed / luck × 1.25 for five rounds at +0x8b..+0x8e, +0x8f silence, +0x90
a fatal hit leaves 1 HP, +0x91 invincible and first for the next round), 34 counter every
physical hit this round (+0x94, 030218), 35 / 36 the battle's EXP / Oz doubled (0601 / 0602).
Genes 122 / 224 poison (+0x89, five rounds of 10% of the most HP; monsters with row +0x2e are
immune).

- **Round** (states 400-899): the commands (Solo: each member's; Pair: one for both, `+0xa48`),
  then each enemy's from its AI lists; a third of the rounds two standing enemies of a two-enemy
  formation act together (`btlord_enemy_rand`). Defend takes hold at once. The queue runs by
  speed: the actors' average (+19998 acting first) plus the action's +6.
- **Damage**: bonus = the pair: +75 holding the Break, then +25 in pair mode, else +50 for actor
  0 (genes: actor 1) / enemies: +50, +125 holding the Break; damage = (bonus + power) × the
  attackers' summed attack / 100 − the target's defense (doubled while defending). Physical: a
  critical (+50 bonus) when random(256) < 10 / 15 / 20 / 50 for the attackers' average level less
  the target's < −49 / < 0 / < 50 / more; otherwise the speed sum less the target's luck must pass
  a check (< −100: 50%, < −49: 60, < 0: 70, < 50: 80, < 101: 90, else 99; always 100 with +15) or
  it's fended off ("Guard!"). The triangle (1 beats 3, 2 beats 1, 3 beats 2) gives × 1.5 (Weak!)
  or × 0.5; then the monster's resistances; the variance (100 − v + random(2v + 1))%. Healing:
  −(bonus + power) × the luck sum / 100, the bonus for the pair +100 with the Break and +50 when
  actor 1 casts, for enemies 150 with the Break else 50.
- **The Break** (+0x12 the gauge −8..8, +0x14 where it's going, +0x18 this round's flags): the
  battle's first event puts it at ±4 for its side; then damage dealt counts 1 an action, a
  critical 1 an action and side, a kill 2, a round in which the holder wasn't hit 1 (`btlord_chk`)
  - but for the holder only kills and untouched rounds count, 1 each. Back to 0 from either side
  is even again. The badge shows the gauge (a ring piece per point, from 0xd1fd0) and Player /
  Enemy / Even.
- **Escape** (`btlact_escape_init`): certain while the pair holds the Break; else 40 / 50 / 60 /
  70% for the pair's average level less the enemies' < −49 / < 0 / < 50 / more (57000 / 57001).
- **Winning** (`btlcalc_chk_dead`, state 0x44b): each enemy's experience and Oz when it falls;
  the results (`MbResultPut`): "Results" (19) at (8, 4), the pair's boards 88 apart, EXP Gained
  (68) at (288, 76) and OZ Earned (69) at (288, 132), the totals at (358, 98 / 154). Each member
  takes the whole battle's EXP - half if down at the end (`MbResultSet`), none if not there.
  Losing (0x4b1) sets flag 0xffd and shows "<leader> lost the battle..." (57016); a tap after
  0x513's picture ends the battle and `btl_lose` (from the frame loop) takes over: a script's
  battle starts the game over (area 0, the title); any other goes back to the pair's Ark - area
  7 / 13 / 19 / 25, room 103 (111 for route 2 once flag 1881 is set, route 3 once 2281 is) at
  (−140, −1, 25) facing 180 - with a fifth of the Oz gone. (Options' Return to Title is
  `btl_retitle`.)

The HUD (`menub_mainPut`; tex2d coordinates + (16, 32) in menu units): the badge (0, 0); the
command buttons (`cmd_bottom`: base (0, 184), face (52 / 96 lit, 184), icon at +8, word at +40)
at (344, 128 + 48k) - Solo, Pair, Escape, Options, then Attack, Genes, Items, Defend; the
targets' plates (`cmd_bottom_teki`, 256 wide, level, name centred at +146, HP bar) at (224, 312 −
48 (n − i)); Back (`cmd_back`) at (208, 320); the members' rows from (0, 328) - portrait (c × 48,
440), HP / MP digits 10 apart (red under a quarter), bars 59 / 49 long - actor 0 40 right and 24
up; the top bar (88..487, 4..28) with the action's name or the target prompt (57066 + scope).
Damage numbers (`__dmg_set` / `__dmg_prc`, 24 frames): 24 × 24 digits 19 apart at (d × 24 + 240
(style & 1), 24 + 24 (style / 2)) - white, red (weak), grey (resisted), green (healing) - with a
hop per digit, and the labels Blowout! Critical! Weak! / No Effect! Guard! Miss! (144 × 24 from
(0, 72)).

Genes (`chk_gene`, `MuCom_IsSkillEquip`, `GetSkillAddrFromPlate`): abilities are items 81..130
(known = owned; action = item + 120, MP its +4), skills 131..186, plates 479..500 (0xd3890: 8 s16
each, three abilities and three skills; 479 / 480 the Human Genes, each character's own 81 + c /
131 + c). The save keeps four plates per character at +0x350 (slot 0 the Human Genes). Pictures:
gene_m001c / m002c / m201c..m220c.mct. A fallen monster, 80 / 40 / 30 / 10% for the attackers'
average level less its < −4 / < 1 / < 6 / more (`btlcalc_0506`, skill 158 −15), teaches the first
not known of the six it carries - its plate's three abilities and three skills, row +0xb4 / +0xb6
/ +0xb8 / +0xba / +0xbc / +0xbe, the later ones once it's level 5 / 10 / 15 / 20 / 25; leaves an
item (rolled first, 20 / 30 / 40 / 50% for their level + 10 less its; then row +0xc0, +0xc2 half
the time from level 20, +0xc4 a third of the time from 40, `btlcalc_0505`); and models 13..72
leave plate 481 + (model − 13) / 3, three models a plate (chk_dead). A
new game (`prty_init_new`, 14 bytes per character at 0xcb3f0: weapon, armor, jewel, plate,
ability, skill) adds 3 Emergency Exits and 2 Chocolates. Messages: 57010 "You got …!", 57049 /
57050 learned a command / skill, 57081 not enough MP.

Skills in battle (`chk_skill_btl`: the pair's from their worn plates, either member's in pair
mode; a monster's from its row's five at +0xaa once it's level 20): 131 / 136 a 10% (20% holding
the Break) chance to nullify a physical / gene hit (`btlque_make_que` → +0xa4 / +0xa5, "Miss!");
132 gene MP / 2; 133 / 138 +5% HP / MP each round (`btlcalc_status_prc`, which also gives the
regen status +10%); 134 a critical x2; 135 a plain attack's first hit critical; 137 a critical
ignores defense; `btlcalc_zoku_0204` scales the damage: 75% for the target's 146 / 147 resistance,
+25 for the attacker's 139 / 140, −25 / +25 for the target's 148..150 / the attacker's 141..143
of the attacker's element (with the triangle flag), −25 for the target's 151 / 152 against a
herbivore / carnivore attacker (row +0x38 / +0x36), +25 for the attacker's 144 / 145 against
one; 153 guard at a quarter of the HP or less (10 / 20%, `btlord_chk_autoguard`); 154 counter a
physical hit (10 / 20%, `btlque_add_counter`); 155 healing x2 (items x20 instead of x10); 156
genes x1.2; 158 −15 off the level difference for genes taught; 159 is read and ignored; 160
escape always works; 161 / 162 experience / Oz + a fifth (pair mode either member's, else the
member queued last, +0xa4a); 163 MP full after the battle. Armor counts too (`chk_gene_btl`, the
fighter's +0x7c): each character's 222..227 (+28 a character) are 148 / 149 / 150 Blaze / Aqua /
Gale Resistance, two of each. The critical roll compares the attackers' average level with the
action's first target's (G+0x7a) for every target. Out of
battle: 164..169 add 10 HP / 1 point a level gained; 175..186 (the Gates) add their item's stat
(`pcparam_max_*`); 170 owned (not worn): the field menu's encounter switch; 171 / 172 encounters
(first strike 50% / no ambushes); 173 shop prices −10%; 174 worn: 1 EXP banked per 10 units walked.

The field menu (`menuf_mainPut` / `menuf_mainPrc`, G+0x3553c open; tex2d coordinates, menu units
= + (16, 32), pieces from window_hd): the corner button, a 40 × 40 box at (456, −32), drawn 32 × 32
at (460, −30) from (432, 992) closed / (464, 992) open. Opening slides the buttons 4 frames from
(460, −28) to their places (closing back): Camp 56 × 56 at (432, 32) from (452, 516), a circle
(460, 60) r 28 (sound 8, `SetCampMainMenu`); Save 40 × 40 at (384, −24) from (468, 576), circle
(404, −4) r 20 (sound 2, var 0 = 0, `SetModeSaveLoadMenu(2)`); Map — maps below 64 only — at
(312, −24) from (512, 388), circle (332, −4) (sound 2, `SetMapMenu`); and with item 170 owned the
encounter switch at (240, −24) from (468, 620) on / (468, 664) off, circle (260, −4): flag 4076
set → buzzer 4, else off (sound 3) / on (sound 2). The pieces' alpha rises 32 a frame (to 320,
drawn at most 255) and falls 32 a frame while G+0x2c0 bit 0 or 3 is set (a scene).

The map screen (`SetMapMenu` → menu 13, `__menuu_MapSet` / `MapPrc` / `MapPut`; `ReadMapMenu(n)`
loads `map_D01`..`map_D04` into Tex2d slot 0x30 at 512 × 512, called with var 0 by `Uty_ScnEffectRead`
— the gate; maps 0–12, 14–28, 30–45, 47–62 are gates 1–4, split by the 3D rooms): MuTex group 1
(table 0xca490, 8 bytes each) picture 13 is the map (0, 0, 480 × 320), 14 the pair's marker
(0, 320, 48 × 48), 15 a marker (0, 368, 16 × 16); both markers blink over 2 frames of 6 each, the
next frame 48 / 16 to the right (`ComUty_PictAnimInit(…, 2, 6, step)`). The pair's marker goes at
table 0xc7b20[map] (s16 x, y; −1 for the 3D rooms) − (24, 20). The story's two markers: the
first of the pair's 25 flags 2601 + 50 × pair + k that is set picks table 0xc7c30[gate][k][0..1]
(s32 x, y, kind; 600 bytes a gate), each at (x − 8, y − 16) when both stay ≥ 0 (kind 1: drawn
nearer, z −1 instead of −3). A tap anywhere (`_pad_chk_updw_box(0, 0, 480, 320)`) closes it,
sound 2.

Effects (`eff3d_set_with_pos_and_ang`, `eff3d_prc`): `eff.bin` (G+0x3bfd4) is 100 × (u32 offset,
u32 size) then the packages (98 used), each a model read with `read_mdl_mem`, up to 32 at once,
placed at a point with draw order + 1000 and deleted when its action ends. The point
(`btlcamera_getpos`): the actor's origin, or the node of the bone whose record **user** word
is the table 0xd37c0's value for that effect (1101 body, 1102 head, 1003 root, 1201.. limbs;
section-6 records carry these users, the NodeDeformer maps the record id to a node). An
effect's first action moves its skeleton (scale 0.001 hides parts; 13 effects have a node or
a few more than segments: the action drives the first ones); its Appearances, diffuse alpha
0 at rest, are coloured by the package action their **refGid** numbers (0-based; −1 none):
9-segment actions, segment 1 a keyType-12 RGBA track, segment 6 a UV translation, segment 8 a
UV scale (the texture coordinates become uv × scale + translation: 29 effects scroll a
texture, e.g. 48's light shafts, 78 / 79's walls of flame). Ids the battle uses: 0x15 casting, 0x18 hit, 0x19 critical, 0x1a a monster falling,
0x1c fixed / MP damage, 0x22 Sacrifice, 0x23 + 3e / 0x24 + 3e a gene of element e on one target
/ over a side (at (500, 0, 523 or 472)), 0x24 statuses, 0x25 + 3e an element's hit (0x25 drains,
poison), 0x2f healing, 0x31 revive (+ e enchant), 0x35 the barrier, 0x36 Analyze.

On the field the same 32 slots serve: `eff3d_set_fld(id, model, loop)` = `eff3d_set(id, model,
0, 100, loop)`, at the model's position (+0x530; the table at 0x5c040 picks a bone instead, as
in battle), scale percent / 100; the slot's +2 is the loop flag (`eff3d_prc` deletes only an
unlooped one at its action's end; `eff3d_delete(id, slot)` marks a slot −2 when it still holds
`id`). `eff3d_set_with_pos_and_ang` also sets the model's angle (+0x53c). Field users: the puzzle
objects (2 + 3 a gem picked up, 3 / 4 an object going / coming, 5 looped on the leader while a
block is held, 7 + 8 a teleport's start and 9 its end on the leader, 7 a toughness tap, 10 a
fragile block breaking, 0xd / 0xc looped / 0xe a candle switch lighting / lit / going out (4.5
above it), 0x19 a squash (5 above the leader)); FieldPCCtrl's footsteps (effect 0 on the leader
with each step sound); call 2023 `Uty_eff3d_set_fld` (GetXmpStkData 0 the effect, 1 an NPC id, 2
a height: once at the NPC, that much above); the cutscene ops 47 `FrmPlayEff` / 56
`FrmPlayEff2` (s16 actor, effect + 99, scale percent: looped, on the leader for actor 0, an NPC
whose id it is, else the current record's point for that actor - 56 also its angle; up to 16
kept with key (effect + 1) × (actor + 1)), 48 `FrmStopEff` (effect + 99: all of it) and 50
`FrmStopEff2` (actor, effect + 99: that one).

On the field `UpdateQuake` (`gameapp_prc`, each frame) hands the offset to `FieldCamera::setOffset`;
`setCameraBack` (painted fields) and `setCamera3D` add it to the viewport's corner, set in whole
pixels. Script op 43 `Quake`: s16 size, from, to, frames - frames < 0 `StartQuake(size, from,
to, frames, held)`, else `EndQuake`.

Screen quakes (`Quake`, `_StartQuake(px, from, to, frames, hold)` / `_EndQuake(px, from, to,
frames)`, updated once a frame by `gameapp_prc`): each frame the x / y offsets are px × (75..100
random)% × now%, against the last frame's direction (and turned once more one time in 11);
`now` runs from → to over the frames, then a passing quake stops while a held one (hold = 1,
mode 2) goes on until EndQuake (a passing StartQuake doesn't replace a held one). `btl_prc`
moves the frustum by the offset (`mceCamera_setPerspectiveFov_op`: (512 − cx + x) / 1024 and
(cy − 384 + y) / 768 of the frustum, i.e. the 1024 × 768 screen's pixels). Every landing hit
of `btlcalc_0404` / `0406` starts (3, 100, 0, 5). Flashes (`_SetFade_back(rgb, from, to, ms)`
with G+0x39c60 = 0: full screen; `PutMask`): alpha from + (to − from) × t / ms, t + 50 a frame,
held at its end unless 0 — over the 3D, under the HUD.

The bosses' staged actions (`btlact_430`..`436`, instead of `btlact_gene_init`; counter +4,
length +8, state +0x98: 0x44c running, 0x76c waiting, 9999 done; cameras at +0x2c, the next set
whenever `btlcamera_prc` has finished the last one — G+0x9e0 cleared once its raw[15] frames
are over). Frames count from 1; the hit (`btlcalc_prc`) at the length, then the wait.

| Action | User | Cameras | Timeline | Hit, wait |
|---|---|---|---|---|
| 430 (m103) | slot cls, then idle | 0x207..0x20a | init effect 0x59 at (500, −4, 500), sound 0x7b; 70: effect 0x5a at (500, −2, 500), 0x7c, quake (3, 0→100, 2, held); 79 / 82: white in / out over 150; 110 / 130: in / out over 500; 133: EndQuake (3, 100→0, 2) | 140, 40 |
| 432 (m102) | slot cls looped (its loop frames) | 0x1fe..0x202, then 0x203 + target (0x205 pair) | init sound 0xbd; 14: 0xc2; 36: effect 0x19 at the user's bone 1206, 0x5b on each target, 0x7d | loop start + 45: effect 0x5c on the targets, 0x7e, the loop plays out; 40 |
| 433 (m104) | slot cls, frames 82..83 looped | the item cameras (0x53 + k, pair 0x55; monsters 0x54 + k / 0x58 / size + 0x57) | init effect 0x15 on the user | 112: camera formation + 12; 40, then idle |
| 434 (m104) | slot cls, then idle | 0x20c..0x20e | init effect 0x1e on the user, 0x62; 84: 0xd4; 86 / 89: white in / out over 150; 100: effect 0x22 on the targets, 0x6d, quake (5, 0→100, 2, held); 128: EndQuake | 130, 65 |
| 435 (m105a) | slot cls, frames 103..113 looped | 0x215..0x21a (on past the hit) | init 0xf0, 0xf1; 103: effect 0x21 at (500, 0, 523), 0x65, white in over 150; 106: out; 133: in over 1500; 163: out over 500 | 163, the loop plays out; 40 |
| 436 (m105a) | slot cls, then idle | 0x210..0x214 (on past the hit) | init 0x83, 0xf0 | 61: quake (10, 100→0, 10); 50 |

Line-ups (`btl_prty_init`, group = G+0x354c0, 1-based): flag 4077 + route set (the story keeps the
pair apart) leaves member 2 out; alone: groups 371 (route 0, Escher), 283 (1, Eluca), 372 (1, Zhamo),
172 / 374 / 376 / 377 (2, Olgar), 350 (2, Vahti), 375 (3, Ayuta); guests (`prty_init_special`:
the leader's level + 5, `_pcparam` + 50 HP / 5 a level, no equipment, no growth - and Speed and
Luck the Defense's, in both releases; +0xa4f): 370 (route 0,
Olgar), 372 (1, Ayuta), 373 (2, Ayuta). The guest acts itself (`btlord_enemy_rand`, counter
+0x17c): 370 Cremate, attack, attack; 372 / 373 Möwe, attack, Asura Lotus. Queue entries repeat
(+0xb): monsters with row +0x3e 2 act twice (the AI chooses again, not the same action), Asura
Lotus (207) 2..5 times (2..3 in pair mode). Steal Item (223, `btlcalc_0504`): if the blow did
damage, (the attackers' luck + 20 − the targets' + power) < −100 / −49 / 0 / 50 / 101 / more gives 40 /
50 / 60 / 70 / 80 / 90%; each target once (+0xa0): row +0xc6 below level 20, +0xc8 or +0xc6 to 39,
+0xca a third of the time from 40 (57005 / 57037 / 57038; skill 157 is read and ignored). Two of the
Human Genes' actions are special in `btlcalc_0404`: Risk Breaker (203) deals 200% (125% when the
user's side doesn't hold the Break), the users first losing a quarter of their HP (Break event 2,
endure, `chk_dead`); Trompe-L'œil (201), when it does damage, rolls 1 in 8 - holding the Break,
the first time in the battle (+0xaa) it comes up it cancels the targets' commands this round (que
+0x399, entry 3 for each) unless the target's row +0x32 is set; not holding it, the first time
(+0xa9) it's Break event 0x20.

Cameras (`btlcamera_prc`): a camera either stands, moves from its first eye / target to the
second (+14..+24) over +30 frames, or turns +28 degrees about its target over them. The menus use
formation + 12 (the first menu), 28 + member / 30 (choosing), a target's (enemy: target + 29, or
size + 32 for bigger ones; the pair 28 + k / 30); attacks slot × 3 + 432 + attacker (the pair's
+2) on the pair's side, random(3) + slot × 4 + 120 / 224 (/ 328 for two) on the enemies'; genes
73 + k / 75, items 83 + k / 85; losing 72 (a slow full turn).

## Puzzle rooms (`PuzzleScene`, `PuzzleManager`, `PuzzleObject`s)

Area 1 (`j001_p01.evt`) holds the 64 puzzle stages, maps 114..177 (the binary's map table has 178
entries; `_get_stgno` = map − 114 while in area 1). `PUZZLE_ROOM_DATA` (0xd5000, filled by its
static initializer at 0x214c) gives each stage 7 words: the touch ability (1 Teleport, 2
Psychokinesis, 4 Toughness, 5 Pyrokinesis - stages 0–15, 16–31, 32–47, 48–63), the ESP count
(−1: no limit, the infinity mark shown), rows (5, along z) and columns (5 or 10, along x), the
grid's corner x, the objects' height y (10) and corner z; cells are 10 units (0xca8d8). Flag 3000
+ stage marks the stage cleared (each pair has four of each ability: `ResetFieldFlag_NewGame`'s
blocks); flag 3300 + stage, cleared by `PuzzleManager::init`, marks the crystal's scene seen.
`crtool puzzles <dataHD>` lists every stage's objects.

The room's NPCs become objects by NPC table +20 bits (`createPuzzleObject`; type, layer):
0x001 NormalBlockTP (teleport rooms) / NormalBlockPK (psychokinesis) (1, 1), 0x002 FixedBlock
(2, 1), 0x004 FragileBlock (3), 0x008 CandleBlock (4), 0x200 RedBlock (5), 0x400 BlueBlock (6),
0x010 CandleSwitch (7), 0x020 WeighSwitch (8, layer 0), 0x080 NormalGem (9), 0x100 / 0x040 /
0x800 WarpCrystalNG / WS / CS (10, layer 2: outside the grid). A grid slot holds one object per
layer; messages (`postMessage`): 5 a switch pressed (data: 1 silent), 6 let go, 7 "can you
teleport?", 8 teleport (`TeleportMessage`: the cell to go to, fade-out / fade-in frames 4 / 4,
waits 6 / 6).

- **Teleport** (`Teleport::onCheckTap`): a tap (`CheckTap`: shorter than 6 frames, off the menu
  buttons) picks the nearest block or gem (`getNearestPuzzleObject`, by its projected top) and,
  within (±17..20, −16..21) of it, swaps places with it: var 13 + 1, the ESP count − 1. At 0
  (unless the count is `ESP_COUNT_INIFINITY`, −1, or the byte at G+0x3553b - never set in the
  game's code - makes `getESPCount` return that) every ability is refused: only
  `SetPunishmentDispOn` (the mark, sound 4). The leader fades out
  (4 frames, effects 7 / 8, sound 0x2b), the object fades out (effect 3), lets go of its switch,
  waits 6, moves to the leader's cell (raised 2.5 on a switch), fades in (effect 4), presses the
  switch; after 6 frames the leader is at the object's old cell (fades in, effect 9, sound 0x2c).
- **Psychokinesis** (`Psychokinesis`, `PsychokinesisDetector`, `NormalBlockPK`): a touch on a
  normal block (within ±20, −16..21 of it) stops the leader (FieldPCCtrl message 2, motion slot 3
  looped, effect 5; `SetPunishmentDispOn` at 0 ESP) and sends the block message 1: sound 0x22, it
  lets go of its switch and rises (+2 a frame, 2 frames, then 5 above its base) and bobs
  (+ sin, 9° a frame). The swipe: inside |dx| < 29, −35 < dy < 20 (480 × 320) nothing; past it
  the angle picks 1 up (row − 1), 2 down, 3 left (column − 1), 4 right; lifting the finger first
  lets go. `onCheckSlide` steps that way while the next cell is on the grid and holds no normal or
  fixed block; no step = message 2 (fall back, free), else message 3 (`PsychokinesisMessage`: the
  cell) and the HUD shows ESP − 1: sound 0x2d, 3 units a frame (distance / 3 frames), then
  `Quake(8)`, the grid slot moved; it falls (−0.8 a frame, sound 0x23) or settles 2.5 up on a
  switch and presses it. A block over the leader on its way (or falling) squashes the try: effect
  0x19 at the leader, sound 0x54, `PuzzleScene::reset`. Landing sends the ability message 4:
  after a slide `decESPCount` (var 13 + 1, ESP − 1 stopping at 0), effect 5 off, the leader free.
  Stages 16–23 have no switches (paths to clear), 24–31 a WarpCrystalWS and one or two switches.
  Holding: `mdl_set_act_motion_loop(leader, 3, 2, 2)` - the field model's action slot 3 (its 17-
  frame fourth built-in action) blended in over 2 frames, then looping from frame 2. Landing:
  `Quake::start(0.5, 100, 0, 8 frames, no hold)` (`NormalBlockPK::onMove`).
- **Toughness** (`Toughness::update`, `FragileBlock`, `RedBlock`, `BlueBlock`): with the leader on
  the grid, a tap on the nearest block-layer object, when it's a fragile (3), red (5) or blue (6)
  one that's there (`isValid` = `ChkNpcDisp`), within the same window: `decESPCount`, effect 7 on
  the leader, sound 0x2f, then message 9 - to a fragile block alone (`npc_kill`, effect 10, sound
  0x29, `removePuzzleObject`), else `postMessage(1, 9)` to the whole block layer: each red / blue
  block changes over. Put away (`npc_kill` then `mdl_set_alpha(0xff, 0x60, 4)`: still drawn - a
  fade sets the model's +0x55c to 1 - but no longer solid; effect 3, sound 0x29) or back
  (`npc_born`, alpha 0x60 to 0xff in 4, effect 4, sound 0x2a; both sounds `NoOverlapSE`, once a
  frame); one coming back with the leader inside its box (position + box offset ± size / 2)
  squashes the try (effect 0x19 at the leader, sound 0x54, `PuzzleScene::reset`). Red blocks
  start solid (unless the stage is cleared), blue ones put away (always). Stages 32–39 are fragile
  blocks, 40–47 red / blue; all are gem rooms with a WarpCrystalNG.
- **Pyrokinesis** (`Pyrokinesis::update`, `CandleSwitch`, `CandleBlock`, `WarpCrystalCS`): a tap
  on a candle switch (7; the nearest block-layer object, same window, no `isValid` test) costs
  one ESP and sends it message 10. The NPC table row's +22..+28 (s16 × 4) belong to these: a
  switch's linked NPC ids (stages 56–63), a candle block's start (+22: 1 lit, 0 put away) and its
  switch's id (+24). `CandleSwitch::update` states: 0 out - message 10: effect 0xd, sound 0x31,
  `SendMessage`, 15 frames to 2; message 11: 5 frames to 1. 1 (`onIgnitionDelay`): then effect
  0xd, sound 0x31 (`NoOverlapSE`), 15 frames to 2. 2: then the flame (effect 0xc, 4.5 above,
  looped) and 3 lit (`onLighted`) - message 10: sound 0x32 (`NoOverlapSE`), the flame off,
  effect 0xe, `SendMessage`, 20 frames to 5; message 11: 5 frames to 4. 4
  (`onExtinguishDelay`): then as message 10 without `SendMessage`. 5: then 0. The message is
  kept until state 0 or 3 reads it. `SendMessage` (anonymous namespace): below stage 56,
  `postMessage(1, 12, &id)` - a candle block whose +24 is the id changes over exactly as a red
  block (sounds 0x2a / 0x29, squashing the leader); from 56, for each linked NPC, message 11 (data
  = the sender's id) to the block-layer object at its cell. Message 11 also starts `draw3D`'s
  beam: 20 frames, two `tex2d_add_tri` (colour 0x600000ff), 1.5 wide, from the sender toward the
  receiver - its length grows with 2.5 × frames. WarpCrystalCS (0x800): hidden unless cleared;
  while any switch on the grid isn't in state 3 a shown crystal goes (fade, effect 3, sound
  0x29), else it comes as WarpCrystalWS does (first time: flag 4000, its event). Stages 48–55 are
  gem rooms (candle blocks and one or two switches, WarpCrystalNG), 56–63 switches only.
- **The pad** (`menuf_mainPut` while G+0x35526 is 1, tex2d units from the anchor `CmdBase` sets,
  (0, 0)): a dark panel (0, 12)–(80, 48) (`tex2d_add4` on 0x2d, 0xb8000000), then from the
  window sheet (0x2c) the infinity mark (792, 58, 72 × 28) at (4, 16) for −1, else "Left" (484,
  184, 30 × 12) at (4, 24) and `cmd_number_puzzle(24, 16, n, 2, swell)`: digits (512 + 28d, 58,
  28 × 28; drawn in 2× units) 20 apart, one digit 20 further right, scaled 1.0–1.4 by the swell
  (`SetFldMenu_puzzle_count` sets 4 when the count changes, counting down a frame). With the
  help state (+0x38: `MfMain_ClrPzlData` 1, `MfMain_SetPzlHelpState`) Retry (420, 184, 44 × 30)
  at (8, 56) - once the count has been above 0, at 0 it shows 5 frames of 11 - and Help (984,
  64, 40 × 32) at (84, 14). `SetPunishmentDispOn(x, y)`: the mark (448, 120, 64 × 64) at the
  touch − 32, drawn a frame in three for 11 frames, the swell set, a sound.
- **"Puzzle Start"** (call 2003 `Uty_PuzzleStart_Init` / `_Proc` / `_Put`): pieces (516, 264) and
  (744, 264) - flag 1: (768, 900) - 224 × 64 of the window sheet at (x1, y1) = (100, 68) and (x2,
  y2) = (156, 140), white, alpha 0. Each frame until flag 0: var 0 = 0 moves x1 −8, x2 +8, alpha
  +0x33; 1 puts them at 60 / 196, opaque; 2 moves on, alpha −0x33. Every area's way into a
  puzzle room sets flag 3070; the puzzle area's script 0 then plays sound 0x43, flags 0, 1 = 0,
  var 0 = 0, the call, 5 frames, var 0 = 1, 30, var 0 = 2, 5, flag 0 = 1, and clears 3070 (a
  second copy: sound 0x45, flag 1 = 1).
- **Gems** (`NormalGem::onIdle`): the leader within half a cell of one picks it up (effect 2,
  sound 0x27): the ESP count + 1.
- **Weigh switches**: pressed by a block standing on them (sound 0x28 unless silent).
- **Crystals**: WarpCrystalNG once no gem is left, WarpCrystalWS while every switch is pressed
  (and gone again, sound 0x29, when one isn't): the first time (flag 3300 + stage clear) flag
  4000, var 1 = the crystal NPC's event, `EvtStart` it - its script runs `Uty_WarpCrystalCamera`
  (call 2014: the view slides to the crystal, which comes in - fade, effect 4, sound 0x2a - var 0
  = 1; the script sets 2, the view slides back, var 0 = 3); afterwards it just comes. A cleared
  stage keeps its crystals and loses its blocks and gems.
- **The pad**: two touch regions (8..52, 56..84) and (84..124, 14..46) start the area's EvrStart 6
  (var 0 = 1 back: `PuzzleScene::reset` - sound 0x44, fade out, the room built again, the leader
  back at its start, fade in) and 7 (var 1 = 666). The update stops while a script holds the
  field (G+0x2c0 bits 0 / 3) or the stage is cleared.

## Field player (`FieldPCCtrl`, 0x3cfd0–0x3dee8)

Behaviour code rather than a file format, recorded here because it ties the formats together.

- **Model:** the party's field models are `f001.bin`–`f008.bin` (the binary names f001/f003/f005/f007,
  one per route). Each package holds four MCAs: stand (60 frames), walk (~30), run (~16) and one more
  (17), each paired with an action that does not fit the skeleton. There is no root motion; all
  movement is code.
- **`operate`:** `CrossKey::getValue` gives a direction (screen x right, y down) and a touch state.
  - State 0: speed 0, action 0. State 1: 0.5 units/frame, action 1. State 2: 1.65 units/frame, action 2.
  - `heading = RepeatPI(atan2(v.x, v.y) + atan2(eye.x − target.x, eye.z − target.z))`; the position moves
    by `speed · (sin h, 0, cos h)` and the model's Y rotation is `h / 2π` turns, so models face +Z.
- **`checkHit`:** `searchNode(pos, 5.0)`, then a floor query within 1000 units sets Y. Floor polygon colours
  (254/0/0, 175/175/175, …) only choose footstep effects. Then
  `RectWrapper::checkHitWallToSphere` with the sphere centre raised by half a height and radius
  `this+0x24` adds the push-out to the position.
- **Radius (`init`/`reset`):** stage record `+68` (s16) when > 0 (3 in dungeons), else 5.0.
- **Start position and angle:** globals that the event script sets (+0x2f940.., degrees at +0x2f94c).

Stage record bytes after the placement rect: `+52..58` 0, `+60` s16 (map id?), `+62` s16 (read
by the background code), `+64` s16, `+66` s16 = 100 (scale %?), `+68` s16 player radius,
`+72` u32 offset, `+76` u32.

## The staff roll (Util 7, `__menuu_StaffRoll*`)

`ModeStaffRollMenu(var 0)`: EndBack0<var 0 + 1>_hd.mct into slot 0x10 (sheet 1, `MuTex_GetTexInfo`
1: 8 shorts a picture at 0xca490). The HD build draws that slot 1:1 on the 1024 × 768 screen:
picture 10 (0, 0, 1024, 1790) at (0, y), y −0.222 a frame (−2.22 fast) down to −1022. A black
panel (tex2d (x − 16, −32), 280 × 384) holds 21 line slots of font block 9 (system messages 60000 +
n; 336 entries; < 3 characters = a gap) at (32, y), 40 apart from 800, rising 2.826 a frame - 10
times a frame while the box (460, −32, 32 × 32: sheet 0 picture 114) is held - each passing −40
taking the next entry at 800. All past and the picture at −1022: 100 frames (windows 3 / 2 off at
5), the panel slides out (3 a frame, 9 fast) below −280, window 4: picture 11 (0, 1936, 1024, 112)
- mode 2: 12 (0, 1824, 1024, 114) - at (0, 328), 4 frames, a tap (`_pad_chk_updw_box(−16, −32,
512, 384)`, sound 2), windows 0 / 1 off, 4 frames, `__mc_SetMenuStat(0xc, 4)`. States (jump
table 0x2b40c): 0 read, 1 windows, 2 scroll, 3 end, 4 panel, 5 card, 6 wait, 7 tap, 8 off, 9 wait.

## The last battle's pair choice (Util 8, `__menuu_LastBt*`)

Flag 0 clear: `StartLastBtMenu` (count and mask 0, no menu). Else round `count`: HP / MP all
restored, texture slot 7 hi / mizu / kaze / mu.mct (0xcf580) - sheet 1 picture 5 (256 × 256) at
(0, 64) in window 2 - window 0 sheet 0 picture 109 at (8, 6), window 1 picture 15 at (0, 56)
under font block 7 entry 0x68 + count at (12, 40) (windows 1 / 2 slide from −140 / −240). A
`MuScrList` (`SetIdInfo(0xd, 4, 7)`) of the pairs whose mask bit is clear, items `ItemRegist(0x1e + 2p)`,
which registers block 7 entry id − 1 = 0x1d + 2p, the leader's name (`ComUty_ComPlate_LastBt`:
plate, icon 4, the name; the later edition's "Final Battle" list shows the leaders too). Chosen: windows off, `Set_Prtyno`,
`ModeCampMenu(1)` + `SetCampMainMenu` (the same camp menu), then font block 7 entry 0x68 + count,
var 0 = `MuStr_GetStrName(7, GetCharNameId(2p))` (block 7 entry 2p + 0x1d: the leader), block 6
entry 0x13 + count on the message board (window 5, from 160 below) and Yes / No (window 6, from
360 right; `ComUty_YesNoChoiceProc`: 2 the first box, Yes; 1 the second). No: back to the list
(windows kept). Yes: `__menuu_LastBtClr` - count + 1, mask |= 1 << p, var 0 = p; at 4 both clear
and so does flag 1. States (0x1f2d4): 0 read, 1 / 2 list (with / without windows), 3 list, 4 off,
5 wait, 6 camp, 7 camp open, 8, 9 question, 10 Yes / No, 11 / 12 off, 13 wait, 14 end. Message
code 1 (as 3) inserts the string a var names.

## The NPC model table (0xcf0ac, `npc_setup` / `EvtNpcMdlRead`)

305 pointers to model packages, indexed by the NPC table row's +4: 0..98 field NPCs and objects
(c*, f*, n*, f0000*), 99..118 the field effect props (fe*), 119..157 more NPCs, 158..196 the
scenes' effect props (ee*: ship, doors, explosions...), 197..215 the scenes' bosses and
creatures (t0152bt, d0151bt..d0557bt; 197 `t1051bt.bin` isn't shipped), 216..223 the party's
battle models (j001..j008), 224..304 the monsters (m*). Every dungeon and town area's rows
501..512 use 198 and 200..214. No row uses 99..197's props, and only `npc_setup` and
`EvtNpcMdlRead` read this table (the only `_read_mdl` callers besides `eff3d`'s `read_mdl_mem`
of eff.bin), so the shipped data never loads them - the same effects come from eff.bin.

Light records (section 3; `mceLight_create` from the package loader only, the hwal light calls
through its function table): 37 bytes after the header - u8 type?, RGB, u8, then f32 intensity /
attenuation-like words (1, 1, 0, 0, 1 or 40, 0) and two −1 words (a target?). 111 packages have
one: 83 black, 9 grey 0x666666 (scene props ee*), 12 white (puzzle blocks f0000blk*, c209, m210), 1
0x6c6c6c, 6 white with the first byte 1 - only the first character's packages (f001, j001, n001,
m001a/b) and c341. A per-package light can't be what shades models (one character would differ
from the other seven), so the port draws full-bright.
Their Appearances (and effects') name actions by refGid; node visibility tracks (keyType 12,
one value) appear in 152 packages but are all 1 except in secondary actions whose segments
outnumber the skeleton (objects that aren't skeleton nodes), so they change nothing drawn.

## Sound effects (`sndeff.bin`)

A table of 256 `{u32 offset, u32 size}` pairs (0x800 bytes) is followed by the data. 253 entries
are CAF files with linear PCM: 22,050 Hz, mono, 16-bit little-endian, 658 s in total.

`_effgo(id)` plays them through OpenAL (16 sources):

| Bits of id | Meaning |
|---|---|
| low 12 | entry |
| 23 | loop |
| 22 | language variant |
| 29 | stop that sound |

The script command `Eff a, b` passes `a | b << 16`, so `Eff 155, 128` is a looping sound 155.

Footsteps (`PlayFootstepSE`, called from `FieldPCCtrl::checkHit`):
- **When:** the walk or run animation passes one of two frames per party: `FOOTSTEP_FRAME`
  (run) {5,13} {6,14} {6,16} {4,10}, and `_FOOTSTEP_FRAME` (walk) {7,22} {6,19} {7,21} {6,19}.
- **Which sound:** one of three, by the floor polygon's colour (`.cmp` polygon +28):
  - 254,0,0 → 27–29;
  - 175,175,175 → 51–53;
  - 125,185,175 → 55–57;
  - 205,220,230 → 59–61;
  - 140,190,190 → 63–65.

Options (`_menuu_OptionSet`, `_MuOptionSetInfo`), saved in the system data:

| Offset | Setting |
|---|---|
| +0x31dd0 | language (0 Japanese, 1 English) |
| +0x31dd1 | BGM volume, 0–100 (÷ 100), default 80 |
| +0x31dd2 | SE volume, 0–100, default 80 |
| +0x31dd3 | bit 0 rotation lock (`AppController shouldAutorotate…`) |
| +0x31dd4 | screen effects on/off |

There is no voice or master volume. `_effplay` skips sounds while the SE volume is 0, unless
id bit 22 is set.

## Later releases (voices, languages)

The iPhone release 2.2.0 (`ChaosRings.app/data`, unencrypted, stripped) and the Vita release
(PCSG00497, a port of it) use the same data formats as 1.0.1 with these additions:
- **Voices:** `voiceevnt.bin` (2,900 story lines) and `voicebtl.bin` (1,220 battle lines), the
  same {u32 offset, u32 size} table as `sndeff.bin`; each clip is headerless signed 16-bit mono
  PCM at 20,000 Hz (clip 0 is silence). Messages start a clip with control code 0x1e. The Vita
  holds the same clips as ATRAC9 files whose sorted names follow the same order
  (`_no_silent`, `j001_D04_0201001_01`, …).
- **Battle voices:** `sys.evt` section +0x40 is 13 speakers × 1000 s16, the `voicebtl.bin`
  clip of each speaker's situation (`[speaker × 1000 + situation − 1]`, 0 none). Actions are 38
  bytes (511): the 1.0.1 fields at +0..+9, six s16 voice situations at +10 (the party's three
  variants, then a monster's), the 1.0.1 +10..+23 at +22..+35, 2 more bytes (420 of the 450
  shared actions are otherwise unchanged). The speaker (`_btlvoice_speaker`) comes from the
  fighter's name id (actor +0xaa: the party's character × 10, +3 for character 7 under flag
  0x997 or flag 0x996 on the fourth pair; monsters their id − 1, ids 71..73 as id + 2 under the
  same flags): 0..7 the characters' (x0..x3, x0..x2 for 3, 5, 7), 80 → 11, 90..93 → 8, 94..98
  → 9, 110..113 → 10, 120..123 → 12; battle models 7, 87, 88 are speaker 7. A line plays with
  tag speaker × 1000 (`__voicego(clip | 0x200000)`), stopping that speaker's last; each actor
  cycles three variants (+0xd3, random at the battle's start). Situations: 1..3 a member's
  commands open (solo, once a round), 4..6 at half HP or less, 13..15 / 16..18 the partner
  down; 7..9 / 10..12 the pair's (and battles 356..359); 0x13..0x15 escaping; 0x16..0x18 hit,
  0x19..0x1b hit at a quarter HP or less, 0x22..0x24 down, 0x1c..0x1e revived (0x1f..0x21 in
  battles 260..262, 280..282, 300, 302, 320..322, 374), 0x25..0x27 healed, 0x28..0x2a to full,
  0x2b..0x2d revived by a gene (0424); attacks 0x2e (struck), 0x31 (critical), 0x34 (one
  fell), 0x37 (a miss / guard), 0x3a (a strong one fell); a round's first action its record's
  +10.. lines; the victory 500..502, 503..505 someone down or at a quarter HP, scripted
  battles' own (0xc93d0: battle, line by pair: 621..630); defeat 0x1f..0x21. A side acting as
  a pair speaks (reacts) through one of the two, picked each round.
- **Languages:** area files have 8 text sections at +0x28 (0 Japanese, 1 English, 2 French,
  3 German, 4 Spanish, 5 Chinese simplified, 6 Chinese traditional, 7 Korean) and `sys.evt` 8
  header entries; 1.0.1 leaves 2–7 at 0.
- 2.2.0's scripts against the Vita's: every entry's code is the same to the byte (`evt.py`
  disassemblies compared), the differences are text, but for `j001_t99.evt`'s boot script
  (return entry 0, the title's start-up): the Vita's runs its own logos and start-up through
  opcodes the iOS releases don't have (0x3f, 0x41, 0x43, 0x44, 0x47..0x4a), so the port keeps the
  data's `j001_t99.evt` when it takes the Vita's scripts over iOS data (the Vita alone: its own). 2.2.0's music is identical to 1.0.1's.
  Neither has `j001_t91.evt` / `j001_d91.evt` (areas 26/27 of 1.0.1's table, test areas).
- **The 2.2.0 code against 1.0.1's** (2026-10-14: the data tables the port takes from 1.0.1
  byte-compared against the 2.2.0 binary, the battle / party / encounter / shop / item code read
  side by side): the tables are the same (gene plates, `_pcparam`, `_exp`, the shop levels and
  lists, treasure pools, the starting party and items, battle cameras, formations and positions,
  the World Select's, every file-name list but for `_hd`) except `TutoTexDisp`'s pictures (the
  same 14 without `_hd`, half size) and the iPhone's screen layouts. The rules are the same - a
  fighter is 0xb8 bytes instead of 0xb0 (+0x20.. kept, G+0x70 → +0x84, an action's +0x16
  affinity → +0x22), several calcs are split into helpers (MP damage, hit roll, the element
  triangle, the resistances; Risk Breaker's ×2 / ×1.25 moved into the precompute), and the
  voices are called from them - with these changes: `pcparam_change_lv` gives a level-up's growth
  for every level of a multi-level battle (1.0.1 one short); a successful escape waits 30 frames
  (10) for its line, which only a success plays (then 40 more; a failure 40); `get_random` is
  only refactored (both: one MT19937 stream - `std::mt19937`'s seeding and output exactly,
  seeded with CFAbsoluteTime's seconds at start, 5489 if never - and a draw below n taking the
  top 31 bits against n doubled past 2^30, rejecting the rest; `get_random(n)` throws away
  r % 3 + 1 draws below 10000, r one more, before its own). The port keeps all of these.
- **The iPhone's screens** (2.2.0; the port's Options, Display, Layout: Auto - the iPhone's with
  2.2.0's data, else the iPad's - iPad or iPhone, `--layout auto|ipad|iphone`). The screen is 3:2
  and shows the 480 × 320 tex2d space: the layout's (32, 64)-(992, 704), scaled × 1.2 about the
  middle (X = W/2 + 1.2 (x − W/2), Y = 1.2 (y − 64)); menu units = tex2d + (16, 32), as on the
  iPad. Code that is the same in both (`WldChoicePut`, `StartPut`, the last battle, Uty_ pictures,
  the battle HUD's tex2d pieces) lands where it is; what 1.0.1 redrew for the iPad's 512 × 384
  (camp, save, shop, results, the title, Yes / No, event boxes) takes 2.2.0's own positions, which
  mostly read as the iPad's moved by (16, 32). Pieces 2.2.0 pins to the screen's corners move by
  (±16, ±32): the field menu's button and bar (−16, +32), the battle badge (+16, +32), the rows
  (+16, −32). Camp has five plates (no Save), Back at (192, 286); the save screen's board picture
  12 at (0, 220), its plates (336, 104) / (336, 160). The message box spans the screen (960 ×
  256 parchment at (32, 2 (y + 16) + 64), the name at (80, 462), the text from (104, …)); the
  staff roll scrolls 17 lines from 320, 20 apart, 1.92174 a frame, its picture 0.125 / 1.25 to
  −575, the fast button (444, 0); Now loading (448, 288); no controller art.
  History (`__menuu_HistoryPut`): 2.2.0 halves the chart and scrolls it sideways as well
  (0..−120): entries from 0xbe3d0 ({x, y, kind, picture} × 41: columns 6 / 158 / 310 / 462, rows
  48..636, the finale (6, 552)) at (h + x, v + 62 + y); the strips (h + 150k, 58) at z −4 over the
  entries (−8), a clear quad (0, 48)-(480, 64) at −4 keeping the chart out from under the black
  bands (0, 0, 480, 48) / (0, 285, 480, 35) at −3; the way-in locks 32 at (x + 54, y − 17) - above
  y 91 the open 31 at (x + 61, y − 10), also every open one; the finale's from 0xbe380 (x − 6 +
  60 / 212 / 365 / 516, open 67 / 219 / 371 / 523); stars from 0xbe3a0 (+66, +66, +244); the
  arrows from 0xbeb40 (218, 92) / (218, 276) / (0, 174) / (469, 174). The finale is two halves
  (29 at x, 30 at x + 294; 1.0.1: x − 72, x + 516, past the iPad screen's sides) or the long "?"
  (cap 13, body 16 up to the mark 14 at x + 252 (1.0.1 + 430), body again to cap 15 at x + 516
  (+ 998)); 2.2.0's sheet has wider caps (its table at 0xbac00: 13 (0, 388, 72, 76), 15 (216, 388,
  72, 76), 16 (72, 388, 36, 76), the rest 1.0.1's halved). Character select (`StartPut`): the
  art (sheet 1 picture 16: 1.0.1's 1024 × 768, 2.2.0's 480 × 320) fills the screen and the
  "Character Select" banner (picture 18: 1024 × 164 / 480 × 84, narrowing downwards) hangs over
  its top, z −4 over −5.
- **Vita audio** (`CR1/sound/*.psarc`: `voiceevnt` 2,900, `voicebtl` 1,220, `bgm` 29 `cr_mNN` - no
  `cr_m07`, like iOS, and no `cr_m31`): RIFF WAVE `.at9` - 'fmt ' WAVE_FORMAT_EXTENSIBLE (0xFFFE),
  block align = the superframe's bytes, sub-format {47E142D2-36BA-4D8D-88FC-61654F8C836C}, then a
  u32 version and the 4-byte ATRAC9 config at +44; 'fact' {u32 samples, u32 overlap delay, u32
  encoder delay (256)}; 'smpl' (music) one loop, start / end inclusive at +44 / +48 of the chunk.
  Voices are 48 kHz mono 72 kb/s, their sorted names (`_no_silent` first) are 2.2.0's clips in
  order - every length matches to the sample at 20 kHz - and 1.67 (story) / 1.51 (battle) times
  louder (RMS) than 2.2.0's. Music is 48 kHz stereo 144 kb/s; the loop's length matches iOS's
  (`_BGM_REPEAT` packet to the file's end) within 35 ms, and the files go on past the loop's
  end into an ending (cr_m30, track 28, plays once and has no loop).
- **Vita sound effects** (`sound/sndeff.bnk`, a Sony SCREAM bank): header u32 version 3, u32 2,
  {u32 offset, size} of the "SBlk" block (0x20) and of the wave data (0xb6c0). SBlk version 9: u16
  sounds / grains / waves at +0x16 / +0x18 / +0x1a (253 / 1,518 / 253); +0x1c the sounds (12
  bytes: u8 volume, u8 grain count at +4, u32 first grain's offset at +8 - six grains each),
  +0x20 the grains (8 bytes: u32 type << 16 | offset; 0x0100 plays the wave header at +0x34's
  table + offset), +0x34 the wave headers (u8 centre note +2 and fine +3 - 0xc4 + 0 = 48 kHz, a
  note a semitone, fine in 128ths: 0xc2 / 0x42 = 44.1 kHz, 0xb6 / 0x42 = 22.05 -, u16 flags +0x0e,
  u32 offset +0x10 and size +0x14 in the data). A wave: u32 subtype (0 mono, 3 stereo - by
  16-byte frame), u32 0x18, then HE-VAG from +0x20: VAG's frames (byte 0 shift | filter low
  nibble << 4, byte 1 flags | filter high nibble << 4; 28 samples) with 128 four-tap filters
  (the first 29 VAG's). The 253 sounds are `sndeff.bin`'s entries 0..252 in order - same lengths;
  often 44.1 kHz or stereo where iOS's are 22.05 kHz mono (`crtool vitasfx`).
- **The Vita's boot** (`j001_t99.evt` return entry 0) uses ten opcodes of its own, their lengths
  read from the code: 0x3f, 0x40, 0x41, 0x43, 0x44 none; 0x47 / 0x49 two u16 (0x47: slot,
  picture); 0x48 / 0x4a four u16 (slot, fade in, hold, fade out: 0, 20, 60, 20) - its logos, then
  the same title as iOS's. The port passes them by (no logos).
- **The Vita's code against 2.2.0's** (2026-10-14, `re/vita`: eboot.elf decompiled, 789 functions
  named from 1.0.1 / 2.2.0): the rules are 2.2.0's - its battle, AI, critical, Break, turn-order,
  random, shop and party code read side by side differ only by inlining, the fighter grown to
  0xc8 bytes (fields + 0x10) and switch layouts; 1.0.1's data tables are there unchanged (moved,
  some inlined as constants: the walk-step frames) but for the screen layouts. What it adds:
  - **Trophies** (16, NPWR08182_00): FUN_8106ae3a, run at every script's end (`__EvtReturn` /
    `__EvtAbort`, after the save opcode 0x46) and in the shop, unlocks trophy n once, a bit of
    G+0x34e38 (kept with the system data): 1..8 the eight gate bosses (any pair's flag 1122 /
    1123 / 1142 / 1143 / 1162 / 1163 / 1182 / 1183 + 400p), 9..12 each pair's primal door (1109 +
    400p), 13 Qualia (1209 + 400p), 14 the businessman (1061 + 400p), 15 the first purchase (a bit
    the shop sets, G+0x34e3d bit 7), 0 all the others.
  - **The Progress Tracker saved as you go**: the same snapshot (`_ContData_Save_set_fld` at a
    script's end, `_ContData_Save_set` before an encounter) is written out at once (opcode 0x45:
    the save file of 0xc24 bytes) - iOS writes it only when the app is left ("Game data is saved
    automatically whenever there is a change of scene", its manual 56036).
  - Its boot opcodes: 0x40 / 0x44 read the system and save data, 0x41 sets the save data up, 0x45
    writes it, 0x46 then checks the trophies; 0x47 / 0x49 load a logo (a table from "sqex.gxt"),
    0x48 / 0x4a show it (fade in, hold, out). Native 2038 (var 9 into G+0x412d2) is new and unused;
    2036 / 2037 are empty.
  - Text: 103 English system texts differ - the controls (buttons, the touchscreen), the manual,
    the credits, and a few corrections (a monster name, an obstacle's description).
- **The Vita's controls** (its manual 56000 / 56025; SceCtrl read each frame, the left stick a
  direction past half): D-pad / left stick move and pick, △ menu, ○ confirm, × cancel, □ skip
  text, L / R pages; puzzles by touch only, SELECT help, START restart. The menus themselves still
  test touch boxes (`_pad_chk_updw_box`: the 960 x 544 screen's touch as x 3.4 + 144 of the
  1920 x 1088 panel). The port takes a controller the same way (README).
- **Vita textures** (`.gxt` in `cr1.psarc`, a zlib PSARC): "MCT_", u32 texture id, one byte,
  then a Sony GXT (magic "GXT", version 0x10000003; texture info at +0x20: data offset, size,
  palette, flags, type at +0x30, format at +0x34, u16 width / height at +0x38). The game's are
  swizzled (type 0): Morton order with the y bit lowest, in square blocks of the smaller side.
  Formats: 0x0C001000 = bytes B G R A, 0x98001000 = bytes B G R (one P8 texture is left).
  Model packages embed the same form. Against 1.0.1's 324 shared textures: 193 identical
  pixels (the palette ones), 115 slightly different (the iPad's PVRTC / RGBA4444 ones, which
  lose their artefacts), 6 within 12/255, 10 redrawn (title, chapter and character-select
  screens and `t0107-b00a00`) - `crtool vita`.

Voices in 1.0.1: nothing in the code or data plays a clip per line of dialogue.
- The `Voice` script opcode is a stub that only skips its 4 bytes. It occurs once in all 29
  event files, against 4,787 `Msg`.
- The only sounds cued right before text boxes are fixed cues: 187 (floating narration text),
  212, 231/232 (screen dims around a box), 18 and 14.
- Battle attack sounds are chosen by the attacker's type (`_btlact_attack_init`: 119 + type,
  132 + (type − 13) / 3, 74 + …). These clips are mostly unvoiced swings and impacts.
- Spectrograms of all 253 entries show no speech (no voiced harmonics with moving formants).

The reference playthrough comes from a different build. Its dialogue scenes have Japanese
speech on nearly every line, and its text boxes show English with Japanese beneath. None of that
speech matches an `sndeff.bin` entry. Voiced dialogue therefore needs that build's own voice
files. The viewer's volume menu can still mark bank entries as voice (T), which routes them to
the Voice slider.

## UI atlas (`window_hd.mct`)

2048×2048 RGBA4444: digits, battle labels, item icons, menu words, portraits and window panels.
Measured from its alpha:
- grey message window (0, 1408) 960×192;
- dark small window (0, 1600) 512×96;
- highlighted window (512, 1600) 512×96;
- round "!" badge (936, 399) 81×83.

### Menu pieces (`ComUty_*`, `MuCom*`)

Menus work in a 512×384 space, doubled for the iPad screen. `tex2d_add` on slot 44 (this
sheet) doubles both the position and the sheet rectangle, so pieces are given at half scale and
drawn 1:1; slot 45 is a plain white surface drawn with vertex colours (0xAARRGGBB). Sheet-0
pictures are `MuTex_GetTexInfo`'s rectangles at 0xc9fb0 (156, `kMenuPicture`); sheets 1 and 2
(0xca490, 0xca530) are full-screen boards and a second atlas.

- **Row** (`ComUty_OptCategoryDraw`): a 24-tall white bar, 100 solid then 123 fading out,
  alpha capped at 0xB3; the label in black at (x + 8, y + 5), 14 units (`MuComFontFromIdRegist`:
  14, or 16 for block 6).
- **Button** (`ComUty_CmdBottom`): base (0,184,24,40) at x + 4, 14 × (28,184,4,40), (28,184,24,40)
  at x + 84; face at (x, y + 4): (996,900,16,32), 20 × (76,188,4,32), (80,188,16,32), or when
  selected (996,932,…), (120,188,…), (124,188,…); the word at (x + 10, y + 8) from a 5 × 88 × 24
  grid at (0, 512): 0 Yes, 1 No, 2 Start, 3 Options, 23 Check, 33 On, 34 Off, 37 Japanese,
  38 English, …
- **Gauge** (`ComUty_SideGuageDraw`): pictures 103, 24 × 105, 104 across 232; 108 / 107 ("Min" /
  "Max") 13 above; a red fill (0xFFFF0000) to the knob (picture 106) at 235 + value × 2.03.
- **Back** (`ComUty_CmdBack` type 2): a white bar fading at both ends, the word (0,224,76,24)
  at x + 80 and arrows (336,880) / (352,880) that bob 0..5; pressed parts turn lime (0xEBFF4B).
- **Windows** (`PutWin`): 40-byte records sliding from a start to a target rect, faded by +36;
  flags +30 choose a frame (full-screen dim, bands, the message panel, a 512×144 board) or,
  with 0x200 as `ComUty_WinDispOn` sets, none - the Options screen draws no frames.

**Boot and title.** The game starts in area 0 (`j001_t99`) and runs its EvrStart 0 (0x2d34; the
table there has 5 entries - slots 5-8 are the start of that script). It fades the Tex2d logos
0-2 (`sqex_hd`, `mve_hd`, `mclogo_hd`) unless `Call 2029` says they were shown, loads the data
(`Call 2021`), shows `title1_hd` (`title2_hd` once flags 45-48 - all four routes - are set) with
music 0, and runs `Util 0`, the title menu. Flag 0 set on return goes back to the logos; else
flag 4089 (new game) is set and `Util 4` (start menu) runs: var 0 = 0 returns to the title,
1-4 picks the world; `Util 5` (save / load) sets var 1 (0 back to Util 4). Then `Call 2022`
(LoadPrty) and a warp: var 1 = 1 → area 6 map 82 (with flag 44), else var 0 = 1-4 → areas 6, 12,
18, 24.

`Util 4`'s exit (`__menuu_StartClr`): var 0 = the pair (1 + cursor on the first sheet, 3 + cursor
on the second), 0 for Back; flag 4092 set on the second sheet. `Util 5` in the title's mode
(`__menuu_SaveLoadSet` / `Put` / `Prc` / `Clr`, mode 1): `Set_Prtyno(var 0 − 1)`; emblems bit k
when flag 0xc9990[pair][k] = {31 + 2p, 32 + 2p, 39} is set, pictures 111 / 112 / 113 at (16, 224),
(88, 224), (160, 224); +0x20 when flags 32, 34, 36, 38 are all set, +0x21 flag 0xc99c0[pair] =
40 + p (clear data), +0x22 a save. Plates (words 0xc9918 = 4, 7, 6) at (328, 136 + 48k): with +0x20
k = 0..2, else with a save 1..2, else 2 alone. 0 asks 57056 (proceed to the last battle?), kind
1; 1 asks 57013, kind 4; 2 asks 57057, kind 2 - or with +0x21 57086..57088 (new game with the
clear data?), kind 3, whose No asks 57089..57091 (a plain new game, clearing the save data), kind
2. Yes plays sound 12; on the way out (`SaveLoadClr`) var 1 = the kind (4: `SaveData_Load_set`;
1: `Set_Prtyno(0)`). `Uty_LoadPrty` (call 2022) is `prty_init_new(var 0 − 1, var 1 == 3 ? 3 :
2)`: the pair, and whether the new game carries the clear data over. `prty_init_new(p, mode)`:
mode 3 first copies the live save (G+0x2f930, 0xc24 bytes) aside (G+0x30554); flags 0xd39f0[p] =
1001 + 400p .. 1400 + 400p are cleared, the pair's bag (+0x580 + 0x100p: 128 u16, item & 0x1ff) is
emptied, and each member (save +0x320 + 0x4c c; +0x48 level, +0x08 HP, +0x0c the HP bonus, +0x14
MP, +0x16 the MP bonus, +0x1c / +0x20 / +0x24 / +0x28 the Attack / Defense / Speed / Luck
bonuses, +0x2a.. the equipment and gene plates) is cleared and set from 0xcb3f0 (7 s16 a
member: level 1, weapon, armor, jewel, the Human Genes plate, ability 81 + c, skill 131 + c - all
added to the bag but a jewel 0 for characters 0, 3, 4, 6; the pair's first member also 0xce700:
items 1, 1, 1, 21, 21), then HP / MP full; mode 3 copies the bonuses, equipment and plates back
from the copy, and the bag, less every item of kind 3 or 9 (+0x11 of its record). Oz (+0xc) is
never touched. Then `ResetFieldFlag_NewGame`: for the pair's four blocks b ({0, 5, 10, 15},
{1, 6, 11, 12}, {2, 7, 8, 13}, {3, 4, 9, 14} for pairs 1..4) flags 3000 + 4b .. 3003 + 4b are
cleared, then flags 3300..3363. Faces: `ComUty_FaceBoardDraw`
at (0, 24 + 88k); on the title with a save `ComUty_GameTimeAndPlayTimeDraw`: a 50% black board
(324, 12, 148 × 64), picture 125 at (372, 12), a white line (325, 28, 146 × 1), the month picture
126 + m at (336, 38), the day (2 digits, 12 apart) at 378, picture 139 at 400, the year at 410,
the time at 368 + {0, 12, 36, 48}, y 58 (the hour's tens left out below 10) with the colon 75 at
(392, 58); `__ComUty_PlayingTimeDraw` at (324, 84): a board 148 × 40, picture 126 at (+20, +4), a
line at +16, the hours (to 999, the hundreds left out when 0) and minutes at +{32, 44, 56, 80, 92},
+22, the colon at (+68, +22).

**Title menu** (`__menuu_Title*`): three plates (`ComUty_ChoicePlateDraw` → `__ComUty_CmdBottom`)
at (328, 114 + 56k): Start (word 2), Scenarios (32), Options (3), the cursor's lit; pictures 155
and 124 at (320, 16) / (380, 16). A plate: base (0,184,24,40), 21 × (28,184,4,40), (28,184,24,40)
at x + 108; face from x + 28: (52,184,24,40), 17 × (76,184,4,40), (80,184,16,40), or selected
(96,…), (120,…), (124,…); icon `kanIconNoTbl[word] - 1` (32 × 32, 16 per row from (0, 376)) at
(x + 8, y + 4); the word at (x + 40, y + 8). Start opens `SetStoreMenu`, Scenarios
`SetHistoryMenu`, Options `SetOptionMenu` + `ModeOptionMenu(1)`.

**Options** (`__menuu_Option*`): the "Options" plate (picture 102) at (8, 4); rows at (20, 56 + 48k);
controls at x 224 / 344 (gauges at (224, 108) and (224, 156)); "1 / 2" from the 14-unit digits
(digit d at (14d, 0), "/" picture 66) at (20, 298); Back at (192, 288). Windows 1 / 3 (rows) slide in
from x −240, 2 / 4 (controls) from +240. Page 1: Instruction Manual (58081, Check), BGM (58085)
and Sound Effects (58086) gauges, Language (58082: Japanese / English), Environment Effects
(58108: On / Off); page 2: Lock Screen Orientation (58109: On / Off), Return to Title (58123) or,
from the title screen, Reset Data (58122). `__MuOptionSetInfo` writes them back: language, the
volumes to AppCtl +0x31dd1 / +0x31dd2, the two flags to +0x31dd4 and bit 0 of +0x31dd3.

Text is drawn with the system font (`font_getfont_utf16`). `font.mct` is only a 256×64 strip.
The port does the same with whatever CJK font the system has (`gfx/font.cpp`); the text is
plain UTF-16 (surrogate pairs allowed), decoded to UTF-8.

Field weather (`ScnEffect`, native 2009): snow, fog, sun and rain overlays (`d0101_snow`,
`d0101_fog`, `d0201_sun`, `d0401_rain01`, …), switched by scripts around warps. Not drawn yet.

## Music (`bgmNN.caf`)

29 tracks (bgm01–bgm31, no bgm07; about 50 minutes). Each is AAC-LC at 44.1 kHz stereo,
about 130 kb/s, in a CAF container with these chunks:
- `desc` (format `aac `, 1024 frames per packet);
- `kuki` (an ESDS whose DecoderSpecificInfo is the AudioSpecificConfig);
- `pakt` (packet count, valid frames, 2112 priming frames, remainder, then the packet sizes as
  big-endian 7-bit varints);
- `data` (a u32 edit count, then the packets).

The original streams them with `GBMusicTrack` (AudioQueue).

- **`_bgmgo(v)`:** bit 29 = stop now; bits 25 / 26 / 27 = fade out over 40 / 10 / 20 frames, then
  stop; otherwise play track `v & 0xff`. Scripts use `Bgm a, b` = `_bgmgo(a | b << 16)`, so
  `Bgm 0, 2048` is a 20-frame fade-out.
- **`setbgm_caf`:** track → file through `_BGM_FILE` (0xcfa40: bgm01…bgm31). The loop start
  (`setRepeatFrame:`) comes from `_BGM_REPEAT` (0xcb4a0), in AAC packets, e.g. bgm01 loops from
  packet 957 (22.2 s). Tracks 28/29 don't loop.
- **Field music:** the last track a script played (+0x35530), resumed by `fldread_prc`. Area
  entry scripts set it (dungeons d01–d04 → tracks 2–5, d05 mostly 17, towns 15, puzzle rooms 6;
  24 appears in many and is probably cutscene music).

The engine decodes with FAAD2. All 29 tracks decode without errors, and bgm01 matches ffmpeg's
decode to within 1 LSB. FAAD2 drops the first frame's output; the loop restart decodes the
packet before the repeat point to prime the decoder.
