# HD textures and HD models

The details behind the README's "Extras": replacement textures and models you make yourself from
your own copy. Both are for personal use: they are made from the game's art - keep them, and
what you make from them, out of anything shared.

## HD textures

Upscaled textures you make yourself, from your own copy - waifu2x or Real-ESRGAN, drawn in place of
the originals:

1. `chaosrings --export-textures` writes every picture the game draws to `<game folder>/textures/export/`
   (`<hash>.png`, upright; the profile folder's `textures/export/` if the game folder can't be written;
   `--export-textures <folder>` elsewhere). Each release exports its own pictures.
2. `python3 tools/hd_textures.py <game folder>` upscales them into `<game folder>/textures/hd/`
   (`<hash>.png`): waifu2x (`waifu2x-ncnn-vulkan` on the PATH, or `--waifu2x <it>`) for the
   lossless and block-compressed pictures, Real-ESRGAN (`--esrgan <the folder with
   realesrgan-ncnn-vulkan and models/>`) for the 16-bit ones - or `--engine waifu2x` for all. It
   needs Python 3 with NumPy and Pillow; `--help` lists the rest (`--scale`, `--noise`, `--only` ...).
3. Start the game: with `textures/hd/` there, HD Textures is on by itself (Options, Detail, HD Textures switches
   it off, and settings.ini keeps that; `--hd-textures` turns it on for a run). One `textures/hd/` can hold every release's.

The exports are ordinary PNGs, so any upscaler or image editor works as well: save the result
under the same name in `textures/hd/`. Keep each picture's shape - one scale across and down, at
least the original's size, up to 8192 a side - and its rows the right way up: a picture that
doesn't fit is left out (the log says which and why) rather than drawn wrong.

An export made with Vita Assets on takes the Vita's lossless version of each picture it has
alike and leaves `<iOS hash>.cralias` files: the tool links each to that picture's upscale, so one
upscale serves the game with or without the Vita's.

`CR_HD_TEXTURES=<folder>` reads them from elsewhere; the older `hd-textures/` beside the program or in
the profile folder is still read while there is no `textures/hd/`. The `tools/hd_textures.py` is the
same in all four ports. Personal use: the outputs are made from the game's art - keep them out of
anything shared.

## HD models

Models you make or improve yourself, drawn in place of the game's - more detailed, smoother,
retextured - moving with the game's own skeleton and animations:

1. `chaosrings --export-models` writes every model to `<game folder>/models/export/` as glTF 2.0
   (`<name>.glb`: its skeleton, meshes, materials, pictures and animations; `--export-models <folder>`
   elsewhere). Blender (File, Import, glTF 2.0) opens them as they are, animations included.
2. Edit it - sculpt, subdivide, rebuild, re-weight, paint - keeping the armature: its bones are named
   `node_<n>...` and the game finds them by that (a new bone goes with its nearest original parent).
   A material keeps the game's render state by its name `cr_mat_<n>...`; give it a picture of your own
   (PNG or JPEG, any size, packed in the `.glb`) and that picture replaces the model's.
3. Export it from Blender as glTF 2.0 (`.glb`, the defaults) to `models/hd/<package>.glb` (`j001.glb` for Escher's `j001.bin`). With HD Models on
   (Options, Detail; off until you switch it on) it is drawn instead of the original from the next scene on.

What it reads: triangles, the first UV set and colour set, four weights a vertex; up to 255 joints a mesh; the game's particle props (`be*` / `ee*`) hang pieces on bones resting at scale 0, which Blender can't keep - edit those outside it. The
animations in the file are for previewing - the game plays its own. `crtool gltfcheck <data> <model.bin> [file.glb]`
checks a round trip (the export loaded back: every model lands exactly on the original in every
animation). `CR_HD_MODELS=<folder>` reads them from elsewhere. Personal use: the exports are the
game's models - keep them, and what you make from them, out of anything shared.

